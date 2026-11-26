#include "ownership.h"
#include "sync_route.h"

#include "device_enrollment.h"
#include "discovery_route_support.h"
#include "price_history_batch_codec.h"
#include "proto_wire.h"
#include "sync_request.h"
#include "provider_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace fortis::wallet::sync {
namespace {

namespace hb = fortis::codec::history_batch;
namespace tj = fortis::codec::tj;
namespace pw = fortis::codec::proto_wire;

constexpr const char* kJsonType = "application/json; charset=utf-8";
// The response envelope around the changes (cursor, asOf, field tags).
constexpr size_t kEnvelopeBytes = 512;

// A response region that grows to kSyncResponseBytes.
struct Growable {
  std::unique_ptr<uint8_t[], base::StorageDeleter> data;
  size_t used = 0;
  size_t capacity = 0;
  bool ok = true;

  bool Reserve(size_t extra) {
    if (!ok) return false;
    if (extra <= capacity - used) return true;
    if (extra > kSyncResponseBytes - used) { ok = false; return false; }
    size_t next = capacity ? capacity : 64 * 1024;
    while (next - used < extra) next *= 2;
    if (next > kSyncResponseBytes) next = kSyncResponseBytes;
    if (!base::ResizeStorage(data, used, next)) { ok = false; return false; }
    capacity = next;
    return true;
  }
  void Append(const void* bytes, size_t length) {
    if (!Reserve(length)) return;
    // An empty source may be a null pointer (a Growable nothing was appended
    // to), which memcpy must not be given even for zero bytes.
    // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
    // cppcheck-suppress arithOperationsOnVoidPointer
    if (length) memcpy(data.get() + used, bytes, length);
    used += length;
  }
  void Text(const char* text) { Append(text, strlen(text)); }
};

void proto_string(Growable& out, uint8_t tag, const char* text, size_t length) {
  out.Append(&tag, 1);
  pw::PutVarint(out, length);
  out.Append(text, length);
}

size_t string_field_size(size_t length) { return 1 + pw::VarintSize(length) + length; }

// Writes a measured encoder's output into the growable tail.
template <typename Write>
bool emit(Growable& out, size_t size, Write write) {
  if (!out.Reserve(size)) return false;
  // Cppcheck 2.13 loses this smart owner's typed element; Clang/GCC reject actual void-pointer arithmetic.
  // cppcheck-suppress arithOperationsOnVoidPointer
  OutBuffer tail{out.data.get() + out.used, size};
  if (!write(tail) || !tail.ok || tail.used != size) { out.ok = false; return false; }
  out.used += size;
  return true;
}

size_t struct_size(const tj::Value* object) {
  OutBuffer counter{};
  return tj::AppendProtoStruct(object, counter) && counter.ok ? counter.used : SIZE_MAX;
}

size_t json_size(const tj::Value* value) {
  OutBuffer counter{};
  return tj::Stringify(value, counter) && counter.ok ? counter.used : SIZE_MAX;
}

void reply(int client, int status, const char* reason, const char* content_type, const uint8_t* body,
           size_t length, const auth::DevicePolicyResult* policy, bool success) {
  http::HttpResponse response{status, reason, content_type, body, length};
  if (policy) {
    response.rate_limit_cost = policy->rate_limit_cost;
    response.rate_limit_remaining = policy->rate_limit_remaining;
  }
  if (success) {
    response.cache_control = "private, no-store";
    response.vary_accept = true;
  }
  WriteHttpResponse(client, response);
}

void reply_text(int client, int status, const char* reason, const char* body, const auth::DevicePolicyResult* policy) {
  reply(client, status, reason, kJsonType, reinterpret_cast<const uint8_t*>(body), strlen(body), policy, false);
}

bool reply_policy(int client, const auth::DevicePolicyResult& policy) {
  if (!policy.status) return false;
  const char* body = policy.error_body ? policy.error_body : "{\"error\":\"INTERNAL\"}";
  http::HttpResponse response{policy.status, policy.reason, kJsonType, reinterpret_cast<const uint8_t*>(body),
                        strlen(body)};
  response.rate_limit_cost = policy.rate_limit_cost;
  response.rate_limit_remaining = policy.rate_limit_remaining;
  response.retry_after = policy.retry_after;
  WriteHttpResponse(client, response);
  return true;
}

// The page visitor: one SyncChange per accepted event.
struct Changes {
  const ScopeAccount* accounts;
  bool protobuf;
  bool include_transfer;
  Growable* out;
  size_t count;
};

// Whether `size` more bytes of changes still fit the response bound.
bool fits(const Growable& out, size_t size) {
  return size <= kSyncResponseBytes - kEnvelopeBytes && out.used <= kSyncResponseBytes - kEnvelopeBytes - size;
}

Visit write_change(void* context, size_t account_index, const tj::Value* event, tj::Arena& arena) {
  auto* changes = static_cast<Changes*>(context);
  // models.includes('transfer') ? page.events.flatMap(…) : []
  if (!changes->include_transfer) return Visit::kWritten;
  const ScopeAccount& account = changes->accounts[account_index];
  const tj::Value* entity = tj::Get(event, "entityKey");
  tj::Value* operation = tj::Get(event, "operation");
  const tj::Value* sequence = tj::Get(event, "sequence");
  const tj::Value* changed_at = tj::Get(event, "changedAt");
  tj::Value* fields = tj::Get(event, "fields");
  if (!tj::IsObject(entity) || !tj::IsString(operation) || !tj::IsObject(fields)) return Visit::kFailed;
  // key: { network, address, ...entityKey }
  tj::Value* key = tj::Obj(arena);
  if (!key || !tj::Set(arena, key, "network", tj::Str(arena, account.network)) ||
      !tj::Set(arena, key, "address", tj::Str(arena, account.address)))
    return Visit::kFailed;
  for (uint32_t i = 0; i < entity->count; ++i) {
    const tj::Member& member = entity->members[i];
    if (!tj::Set(arena, key, member.key, member.key_length, member.value)) return Visit::kFailed;
  }
  char version[48];
  snprintf(version, sizeof(version), "s2_%s", sequence->text);
  char changed[32];
  if (!fortis::codec::provider_text::IsoString(static_cast<int64_t>(changed_at->number), changed)) return Visit::kFailed;
  Growable& out = *changes->out;
  if (!changes->protobuf) {
    tj::Value* change = tj::Obj(arena);
    if (!change || !tj::Set(arena, change, "model", tj::Str(arena, "transfer")) ||
        !tj::Set(arena, change, "key", key) || !tj::Set(arena, change, "operation", operation) ||
        !tj::Set(arena, change, "version", tj::Str(arena, version)) ||
        !tj::Set(arena, change, "changedAt", tj::Str(arena, changed)) || !tj::Set(arena, change, "fields", fields))
      return Visit::kFailed;
    const size_t size = json_size(change);
    if (size == SIZE_MAX) return Visit::kFailed;
    if (!fits(out, size + 1)) return Visit::kFull;
    if (changes->count) out.Text(",");
    if (!emit(out, size, [&](OutBuffer& tail) { return tj::Stringify(change, tail); })) return Visit::kFailed;
  } else {
    const size_t key_size = struct_size(key);
    const size_t fields_size = struct_size(fields);
    if (key_size == SIZE_MAX || fields_size == SIZE_MAX) return Visit::kFailed;
    const size_t version_length = strlen(version), changed_length = strlen(changed);
    // SyncChange: model, key, operation (omitted when empty), version,
    // changed_at, fields; the message fields are always set.
    const size_t body = string_field_size(8) + 1 + pw::VarintSize(key_size) + key_size +
                        (operation->length ? string_field_size(operation->length) : 0) +
                        string_field_size(version_length) + string_field_size(changed_length) + 1 +
                        pw::VarintSize(fields_size) + fields_size;
    if (!fits(out, 1 + pw::VarintSize(body) + body)) return Visit::kFull;
    const uint8_t change_tag = 0x3a;
    out.Append(&change_tag, 1);
    pw::PutVarint(out, body);
    proto_string(out, 0x0a, "transfer", 8);
    const uint8_t key_tag = 0x12;
    out.Append(&key_tag, 1);
    pw::PutVarint(out, key_size);
    if (!emit(out, key_size, [&](OutBuffer& tail) { return tj::AppendProtoStruct(key, tail); })) return Visit::kFailed;
    if (operation->length) proto_string(out, 0x1a, operation->text, operation->length);
    proto_string(out, 0x22, version, version_length);
    proto_string(out, 0x2a, changed, changed_length);
    const uint8_t fields_tag = 0x32;
    out.Append(&fields_tag, 1);
    pw::PutVarint(out, fields_size);
    if (!emit(out, fields_size, [&](OutBuffer& tail) { return tj::AppendProtoStruct(fields, tail); }))
      return Visit::kFailed;
  }
  ++changes->count;
  return out.ok ? Visit::kWritten : Visit::kFailed;
}

}  // namespace

Route::~Route() { Stop(); }

bool Route::Start(const storage::KvStore* kv, address::Store* store, bool attestation_required, base::Clock* clock) {
  if (started_ || !kv || !store || !clock) return false;
  if (!scope_.Start(store)) return false;
  kv_ = kv;
  attestation_required_ = attestation_required;
  clock_ = clock;
  started_ = true;
  return true;
}

void Route::Stop() {
  if (!started_) return;
  scope_.Stop();
  started_ = false;
}

bool Route::Register(http::HttpRouter& router) { return started_ && router.Add("POST", "/v1/sync", Handle, this); }

void Route::Handle(int client, const http::HttpRequest& request, bool, void* context) {
  static_cast<Route*>(context)->Serve(client, request);
}

// POST /v1/sync — one device-scoped page of transfer changes.
//
// Reproduces the earlier TypeScript implementation: its sync route,
// SyncFacet.read, the device sync scope, the account sync journal and the
// /v1 hooks, with proto sync.proto; the caller is the
// app's IntelligenceSyncClient.fetch (a protobuf SyncRequest with models
// ["transfer"], limit 200 and the cursor when it has one, Accept protobuf;
// it pages again at once on hasMore and keeps its cursor until a batch is
// applied).
//
// Method and path: POST /v1/sync only; any other method or spelling (GET,
// PUT, a trailing slash, another case) is a router miss (its own 404 body).
// A query string is ignored.
//
// Order of checks, each answering before the next runs:
//  1. Request reader: a body over the router's bound is 413.
//  2. The preHandler hook (auth-first: before the body is judged): any
//     Origin header (even empty or "null") 403 BROWSER_ORIGIN_FORBIDDEN;
//     then the bearer: no "Bearer " prefix (the scheme is case-sensitive) or
//     a token outside fintel_dev_ + 43 base64url characters 401
//     UNAUTHORIZED, an unknown token 401 UNAUTHORIZED, an expired or revoked
//     device 401 SESSION_EXPIRED, and, with ATTESTATION_MODE=required, a
//     device without App Attest or Play Integrity 401 ATTESTATION_REQUIRED
//     (none of these carry rate headers); then the per-minute device quota
//     at cost 1 (600/1,200/240/120 by tier): from here every answer carries
//     x-rate-limit-cost: 1 and x-rate-limit-remaining; over the quota 429
//     RATE_LIMITED with retry-after: 60.
//  3. Body parser: application/json (any case, parameters allowed; one
//     leading BOM dropped; duplicate keys keep the last value; empty or
//     invalid JSON 400 {"error":"BAD_REQUEST"}); application/protobuf (any
//     case, parameters allowed; a Buffer). Other types, such as text/plain,
//     application/x-www-form-urlencoded, application/x-protobuf,
//     application/octet-stream or a body without a Content-Type, are 415
//     {"error":"UNSUPPORTED_MEDIA_TYPE"}. No type and no body leaves the
//     body undefined.
//  4. Body schema (ajv): the body must be an object (a Buffer passes
//     untouched, so protobuf skips this step); other keys are removed;
//     cursor and after must be strings after coercion, after an ajv
//     "date-time", models an array (a scalar becomes [scalar]) of
//     "transfer", limit an integer 1..500 (strings, booleans and null
//     coerce). The first failure is 400 {"error":"BAD_REQUEST"}.
//  5. The handler: a protobuf body is decoded (fromBinary: unknown fields
//     skipped, the last cursor/after/limit wins, models repeat, invalid
//     UTF-8 or wire 400 {"error":"BAD_REQUEST"}); an empty models list
//     counts as absent. Then zod: cursor .trim().max(64 UTF-16 units),
//     after z.iso.datetime({offset: true}), models .min(1).max(1) of
//     "transfer", limit .int() 1..500 (default 200), and cursor and after
//     are mutually exclusive. Any failure is 400 {"error":"BAD_REQUEST"}.
//
// The read (SyncFacet.read at one `now`): the scope is the accounts of the
// device's live watches — a portfolio watch's account id and the (network,
// address…) of an `address:` topic (the address keeps further colons); a
// topic without both parts, another kind, a null account, an unknown
// address or an expired watch adds nothing, and one account counts once.
// A non-empty scope is cached for four minutes per device (a watch written
// through rememberTopic/remember drops it at once; a plain WatchStore.put
// does not). Resolving renews watches within five minutes of expiry
// (fire-and-forget). The cursor is the request's (trimmed), else `after`
// becomes a read from sequence zero, else the request is a handshake.
// `after` in the future or more than six days old answers an empty page
// with reset.
//
// The journal page (AccountSyncLog.read): the head is read first as the
// watermark. No cursor: an empty page at the head. A cursor that is not
// exactly s2_{13 digits}_{20 digits} (ASCII; including "" and whitespace
// trimmed to "") or whose issuedAt is in the future, more than six days old
// or whose sequence is past the head: an empty page with reset: true at the
// head. Otherwise the scope's events with sequence in (cursor, watermark]
// merge in global order, `after` keeps only changedAt > after, limit + 1
// are read to set hasMore, and the next cursor is s2_{last event's commit
// time}_{its sequence} when hasMore, else s2_{now}_{watermark}. Cursors are
// not signed or bound to a device: a lowered sequence replays the device's
// own events again, another device's cursor reads only the caller's scope,
// and an old cursor answers the same page again (plus anything written
// since). `after` is not carried by the next cursor. A hasMore cursor ages
// from its last event's commit, so it can be stale six days later.
//
// The answer, `private, no-store` and `vary: Accept`: protobuf only for an
// Accept naming application/protobuf with q > 0 (any case, any position),
// otherwise JSON (also for every error). JSON:
// {"schema":1,"cursor","hasMore","reset","asOf","supportedModels":
// ["transfer"],"changes":[{"model":"transfer","key","operation",
// "version":"s2_{sequence}","changedAt","fields"}]}; key is {network,
// address, ...entityKey} in JavaScript property order (array-index names
// first; an entityKey network/address overrides the account's value in its
// place); operation is the stored upsert/patch/delete, so a delete is a
// tombstone whose fields are stored as written (often {}); explicit nulls
// stay. SyncResponse protobuf: schema, cursor, has_more and reset only
// when true, as_of, supported_models, changes (key and fields always set as
// google.protobuf.Struct, operation omitted when empty).
//
// Persistence and concurrency: the journal is append-only — each writer
// frame (address_store.h UpsertTransfers / AppendSyncEvents, under the
// store's writer mutex) holds its events and the head together, so a crash
// keeps a frame whole or drops it and the journal stays 1..head; events
// expire seven days after commit; nothing per device is stored by a read.
// Reads take no writer lock: the watermark read first bounds the page, so a
// read during appends returns a prefix of what a later read returns. The
// scope cache and the quota counters are process memory (empty after a
// restart, as in TypeScript).
//
// Native bounds and differences (asserted by tests/sync_edges_test.*):
// a JSON body over 256 KiB, 8,192 tokens or depth 16, a number over a
// double or a lone-surrogate escape is refused by the request JSON parser
// (413 BODY_TOO_LARGE or 400 BAD_REQUEST, after the device policy); a page
// whose response would pass 2 MiB ends early with hasMore (a single change over it is a 500); the scope cache is
// direct-mapped (two devices sharing a slot recompute early, never read
// each other's scope); x-rate-limit-remaining counts the wall-clock minute;
// no last_seen_at presence write.
void Route::Serve(int client, const http::HttpRequest& request) {
  // The auth hook first (origin, bearer, attestation, quota at cost 1), then
  // content-type parsing and the body schema (auth-first).
  const auth::DevicePolicyResult policy = auth::AuthorizeDeviceRequest(request, 1, attestation_required_);
  if (reply_policy(client, policy)) return;
  std::unique_ptr<codec::BoundedJsonToken[], base::StorageDeleter> token_storage;
  codec::BoundedJsonToken* tokens = nullptr;
  if (request.json_content_type) {
    token_storage = base::AllocateStorage<codec::BoundedJsonToken[]>(sizeof(codec::BoundedJsonToken) * codec::kBoundedJsonMaxTokens);
    tokens = token_storage.get();
    if (!tokens) {
      reply_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", &policy);
      return;
    }
  }
  discovery::DiscoveryBody body;
  const int framing = discovery::ParseDiscoveryBody(request, tokens, tokens ? codec::kBoundedJsonMaxTokens : 0, &body);
  if (framing) {
    token_storage.reset();
    reply_text(client, framing,
               framing == 415 ? "Unsupported Media Type" : framing == 413 ? "Payload Too Large" : "Bad Request",
               framing == 415   ? "{\"error\":\"UNSUPPORTED_MEDIA_TYPE\"}"
               : framing == 413 ? "{\"error\":\"BODY_TOO_LARGE\"}"
                                : "{\"error\":\"BAD_REQUEST\"}",
               &policy);
    return;
  }
  tj::Arena arena = tj::GrowingArena(kSyncArenaBytes);
  std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
  Input input;
  const bool protobuf_body = body.kind == discovery::DiscoveryBodyKind::kProtobuf;
  const bool schema_ok =
      protobuf_body || CheckSchema(body.kind == discovery::DiscoveryBodyKind::kJson ? &body.json : nullptr, arena, &input);
  // CheckSchema copied every value it kept into the arena.
  token_storage.reset();
  if (!schema_ok) {
    tj::FreeArena(arena);
    reply_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", &policy);
    return;
  }
  if (protobuf_body && !DecodeProto(body.bytes, body.length, arena, &input)) {
    tj::FreeArena(arena);
    reply_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", &policy);
    return;
  }
  Request read;
  if (!Validate(input, &read)) {
    tj::FreeArena(arena);
    reply_text(client, 400, "Bad Request", "{\"error\":\"BAD_REQUEST\"}", &policy);
    return;
  }

  // SyncFacet.read(deviceId, { cursor, after, models, limit })
  const int64_t now = clock_->NowMs();
  char as_of[32];
  ScopeAccount* accounts = nullptr;
  size_t account_count = 0;
  bool ok = fortis::codec::provider_text::IsoString(now, as_of) &&
            scope_.Accounts(policy.device.id, now, arena, &accounts, &account_count);
  const char** ids = ok && account_count ? static_cast<const char**>(arena.Alloc(sizeof(char*) * account_count))
                                         : nullptr;
  if (account_count && !ids) ok = false;
  for (size_t i = 0; ok && i < account_count; ++i) ids[i] = accounts[i].id;
  // A cursor, else a timestamp read from sequence zero, else the handshake.
  char cursor[kCursorTextBytes];
  const char* cursor_text = nullptr;
  size_t cursor_length = 0;
  if (read.has_cursor) {
    cursor_text = read.cursor;
    cursor_length = read.cursor_length;
  } else if (read.has_after) {
    FormatCursor("00000000000000000000", now, cursor);
    cursor_text = cursor;
    cursor_length = strlen(cursor);
  }
  const bool stale_after = read.has_after && (read.after_ms > static_cast<double>(now) ||
                                              static_cast<double>(now) - read.after_ms > kCursorMaxAgeMs);
  Growable changes_out;
  Changes changes{accounts, AcceptsProtobuf(request), read.include_transfer, &changes_out, 0};
  PageResult page;
  Journal journal(kv_);
  if (ok) {
    ok = stale_after ? journal.Read(nullptr, 0, "invalid", 7, read.limit, now, false, 0, write_change, &changes,
                                    arena, &page)
                     : journal.Read(ids, account_count, cursor_text, cursor_length, read.limit, now, read.has_after,
                                    read.after_ms, write_change, &changes, arena, &page);
  }
  tj::FreeArena(arena);
  Growable out;
  if (ok && !changes.protobuf) {
    out.Text("{\"schema\":1,\"cursor\":\"");
    out.Text(page.cursor);
    out.Text(page.has_more ? "\",\"hasMore\":true" : "\",\"hasMore\":false");
    out.Text(page.reset ? ",\"reset\":true,\"asOf\":\"" : ",\"reset\":false,\"asOf\":\"");
    out.Text(as_of);
    out.Text("\",\"supportedModels\":[\"transfer\"],\"changes\":[");
    out.Append(changes_out.data.get(), changes_out.used);
    out.Text("]}");
  } else if (ok) {
    // SyncResponse: schema, cursor, has_more and reset when true, as_of,
    // supported_models, changes.
    const uint8_t schema[] = {0x08, 0x01};
    out.Append(schema, sizeof(schema));
    proto_string(out, 0x12, page.cursor, strlen(page.cursor));
    const uint8_t has_more[] = {0x18, 0x01}, reset[] = {0x20, 0x01};
    if (page.has_more) out.Append(has_more, sizeof(has_more));
    if (page.reset) out.Append(reset, sizeof(reset));
    proto_string(out, 0x2a, as_of, strlen(as_of));
    proto_string(out, 0x32, "transfer", 8);
    out.Append(changes_out.data.get(), changes_out.used);
  }
  if (!ok || !changes_out.ok || !out.ok) {
    reply_text(client, 500, "Internal Server Error", "{\"error\":\"INTERNAL\"}", &policy);
    return;
  }
  reply(client, 200, "OK", changes.protobuf ? "application/protobuf" : kJsonType, out.data.get(), out.used, &policy, true);
}

}  // namespace fortis::wallet::sync
