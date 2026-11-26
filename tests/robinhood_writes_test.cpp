#include "test_support.h"
#include "strict_unit_expectations.h"
// Unit checks of the three Robinhood transaction POST routes (R43 prepare,
// R44 submit, R45 diagnose) below the HTTP layer, against the TypeScript
// oracles robinhood_prepare_edges.json, robinhood_submit_edges.json and
// robinhood_diagnose_edges.json:
//  - every request record that reached the handler (past the body parser and
//    the device hook) goes through the route's own steps — protobuf-es
//    decoding (RobinhoodDecodeWriteRequest), zod (RobinhoodValidWriteRequest),
//    the trade policy (ValidateTradeCalls over the fixture catalog's active
//    stocks) — and must reach TypeScript's verdict: a bare 400 without any
//    provider request, the unconfigured 503, a policy 400 after the catalog
//    read only, or the Alchemy request TypeScript sent;
//  - every Alchemy request body is rebuilt (RobinhoodRpcEnvelope over
//    RobinhoodPrepareParams / RobinhoodSimulateParams / RobinhoodTraceParams /
//    AttachSignatures of the ticket's stored operation) and must equal the
//    recorded body byte for byte;
//  - every answer built from a recorded reply is rebuilt (prepare: ticket from
//    the counter, expiresAt, validatePrepared; submit: the result; diagnose:
//    buildReport / failedReport at the record's instant) and must equal
//    TypeScript's JSON text, or its protobuf bytes for protobuf answers;
//  - every 200 JSON answer round-trips (Parse → Stringify) and re-encodes
//    (RobinhoodEncodeDynamic) to the exact bytes dynamicMessage + toBinary
//    give for it;
//  - the TicketStore: expiry boundaries, device binding, count refusal,
//    single use, in-flight, failure release, prune-while-in-flight, replace,
//    the 1,000 cap, the 16 MiB bound and concurrent claims and inserts.
// Usage: robinhood_writes_test prepare.json submit.json diagnose.json
#include "strict_proto_cases.h"
#include "cross_chain_support.h"
#include "discovery_post_unit.h"
#include "robinhood_markets.h"
#include "robinhood_policy.h"
#include "robinhood_service.h"
#include "robinhood_tickets.h"
#include "provider_text.h"

#include <mbedtls/sha256.h>
#include <pthread.h>

#include <map>

using namespace fortis::exchange;

namespace {

using namespace post_unit;
namespace rh = fortis::exchange::robinhood;
namespace cc = fortis::exchange::cross_chain;
using tj::Value;

struct Counts {
  int handled = 0, zod_refusals = 0, unconfigured = 0, policy_refusals = 0, upstream_bodies = 0, answers = 0,
      encoded = 0, round_trips = 0, skipped = 0;
};

bool configured(const Value* env, const char* name) {
  const Value* value = tj::Get(env, name);
  return tj::IsString(value) && value->length;
}

std::string stringify(const Value* value) {
  tj::OutBuffer measure{nullptr, 0, 0, true};
  if (!tj::Stringify(value, measure) || !measure.ok) return "<stringify failed>";
  std::string out(measure.used, '\0');
  tj::OutBuffer buffer{reinterpret_cast<uint8_t*>(&out[0]), out.size(), 0, true};
  if (!tj::Stringify(value, buffer) || !buffer.ok) return "<stringify failed>";
  return out;
}

std::string encode_dynamic(const Value* value) {
  tj::OutBuffer measure{nullptr, 0, 0, true};
  if (!RobinhoodEncodeDynamic(measure, value) || !measure.ok) return "<encode failed>";
  std::vector<uint8_t> bytes(measure.used ? measure.used : 1);
  tj::OutBuffer buffer{bytes.data(), measure.used, 0, true};
  if (!RobinhoodEncodeDynamic(buffer, value) || !buffer.ok) return "<encode failed>";
  return to_hex(bytes.data(), buffer.used);
}

int status_of(const Value* record) {
  const Value* status = tj::Get(record, "status");
  return tj::IsNumber(status) ? static_cast<int>(status->number) : 0;
}

bool has_media(const Value* record, const char* media) {
  const std::string type = text(header(tj::Get(record, "requestHeaders"), "content-type"));
  std::string bare = type.substr(0, type.find_first_of("; \t"));
  return strcasecmp(bare.c_str(), media) == 0;
}

// The calls a record made to a host substring and JSON-RPC method.
std::vector<const Value*> calls_to(const Value* record, const char* method) {
  std::vector<const Value*> out;
  const Value* upstream = tj::Get(record, "upstream");
  for (uint32_t i = 0; tj::IsArray(upstream) && i < upstream->count; ++i) {
    const std::string body = text(tj::Get(upstream->items[i], "body"));
    if (body.find(std::string("\"method\":\"") + method + "\"") != std::string::npos) out.push_back(upstream->items[i]);
  }
  return out;
}

bool read_catalog(const Value* record) {
  const Value* upstream = tj::Get(record, "upstream");
  for (uint32_t i = 0; tj::IsArray(upstream) && i < upstream->count; ++i)
    if (text(tj::Get(upstream->items[i], "url")).find("/rhj/assets") != std::string::npos) return true;
  return false;
}

// The reply body a recorded call got, parsed as the transport parses a 2xx
// JSON body; null for a non-2xx, unparseable or timed-out (a reply held
// past the 10 s abort) reply: a transport failure.
const Value* reply_of(tj::Arena& arena, const Value* record, const Value* call) {
  const Value* entries = tj::Get(tj::Get(record, "providers"), text(tj::Get(call, "key")).c_str());
  for (uint32_t i = 0; tj::IsArray(entries) && i < entries->count; ++i) {
    const Value* delay = tj::Get(entries->items[i], "delayMs");
    if (tj::IsNumber(delay) && delay->number >= 10000) return nullptr;
  }
  const Value* status = tj::Get(call, "status");
  if (!tj::IsNumber(status) || status->number < 200 || status->number > 299) return nullptr;
  const Value* reply = tj::Get(call, "reply");
  if (!tj::IsString(reply)) return nullptr;
  return tj::Parse(arena, reinterpret_cast<const uint8_t*>(reply->text), reply->length);
}

// The fixture catalog's active stocks, as ActiveStocks builds them.
rh::StockSet active_stocks(tj::Arena& arena, const Value* fixture) {
  rh::StockSet stocks;
  const Value* raw = tj::Get(fixture, "catalog");
  const Value* body = tj::Parse(arena, reinterpret_cast<const uint8_t*>(raw->text), raw->length);
  const Value* assets = rh::ParseAssets(arena, body);
  auto* tokens = static_cast<rh::StockToken*>(arena.Alloc(sizeof(rh::StockToken) * (assets->count + 1)));
  size_t count = 0;
  for (uint32_t i = 0; i < assets->count; ++i) {
    const Value* asset = assets->items[i];
    if (!tj::StringEquals(tj::Get(asset, "status"), "active")) continue;
    const Value* contract = tj::Get(asset, "contractAddress");
    for (size_t c = 0; c < 42; ++c) {
      const char ch = contract->text[c];
      tokens[count].address[c] = (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch | 0x20) : ch;
    }
    tokens[count].address[42] = '\0';
    tokens[count].symbol = tj::Get(asset, "tokenSymbol");
    ++count;
  }
  stocks.items = tokens;
  stocks.count = count;
  return stocks;
}

std::string ticket_for(long long counter) {
  char seed[64];
  const int length = snprintf(seed, sizeof(seed), "robinhood-ticket-%lld", counter);
  uint8_t digest[32];
  mbedtls_sha256(reinterpret_cast<const unsigned char*>(seed), static_cast<size_t>(length), digest, 0);
  char ticket[33];
  if (!RobinhoodTicketText(digest, ticket)) return "";
  return ticket;
}

// The request body as the route reads it (JSON or protobuf; anything else
// is "some other value", which no schema accepts).
const Value* input_of(tj::Arena& arena, const Value* record, RobinhoodWriteKind kind) {
  const std::vector<uint8_t> bytes = unhex(tj::Get(record, "bodyHex"));
  if (has_media(record, "application/json")) return tj::Parse(arena, bytes.data(), bytes.size());
  if (has_media(record, "application/protobuf")) return RobinhoodDecodeWriteRequest(kind, bytes.data(), bytes.size(), arena);
  return nullptr;
}

void compare_answer(const Value* record, const Value* answer, const std::string& label, Counts& counts) {
  ++counts.answers;
  if (tj::Get(record, "wireHex")) {
    const auto actual = encode_dynamic(answer), expected = text(tj::Get(record, "wireHex"));
    size_t difference=0;while(difference<actual.size() && difference<expected.size() && actual[difference]==expected[difference]) ++difference;
    check(actual == expected, label + ": protobuf answer differs", "at "+std::to_string(difference)+" native "+actual.substr(difference,120)+" expected "+expected.substr(difference,120));
  } else {
    const std::string want = text(tj::Get(record, "responseText"));
    // sendProtobufOrJson sends a string payload as text/plain.
    const std::string got = tj::IsString(answer) ? text(answer) : stringify(answer);
    check(got == want, label + ": answer differs", "\n  native " + got.substr(0, 400) + "\n  oracle " + want.substr(0, 400));
  }
}

void compare_body(const Value* call, Value* envelope, const std::string& label, Counts& counts) {
  ++counts.upstream_bodies;
  const std::string want = text(tj::Get(call, "body"));
  const std::string got = envelope ? stringify(envelope) : "<null>";
  check(got == want, label + ": Alchemy request body differs",
        "\n  native " + got.substr(0, 600) + "\n  oracle " + want.substr(0, 600));
}

// ---- per route ------------------------------------------------------------------------------------------

struct Context {
  const Value* fixture = nullptr;
  rh::StockSet stocks;
  std::map<std::string, const Value*> groups;          // name → env
  std::map<std::string, const Value*> prepared_by_ticket;  // ticket → stored operation (JSON)
};

void prepare_record(tj::Arena& arena, Context& context, const Value* record, const Value* input, Counts& counts) {
  const std::string label = "R43 " + text(tj::Get(record, "name"));
  const Value* env = context.groups[text(tj::Get(record, "group"))];
  const int status = status_of(record);
  if (!configured(env, "ALCHEMY_API_KEY") || !configured(env, "ALCHEMY_GAS_POLICY_ID")) {
    ++counts.unconfigured;
    check(status == 503 && text(tj::Get(record, "responseText")) == "{\"error\":\"SPONSORED_TRADES_UNAVAILABLE\"}" &&
              tj::Get(record, "upstream")->count == 0,
          label + ": unconfigured prepare is not TypeScript's 503");
    return;
  }
  const auto wallet = calls_to(record, "wallet_prepareCalls");
  if (!read_catalog(record) && wallet.empty() && status != 400) { ++counts.skipped; return; }  // outage, breaker
  Value* from_lower = cc::Lower(arena, tj::Get(input, "from"));
  bool exhausted = false;
  Value* executable = rh::ValidateTradeCalls(arena, tj::Get(input, "calls"), context.stocks, from_lower->text,
                                             from_lower->length, &exhausted);
  if (wallet.empty()) {
    if (status == 400 && text(tj::Get(record, "group")) != "outage") {
      ++counts.policy_refusals;
      check(!executable, label + ": the policy accepts calls TypeScript refused");
    } else {
      ++counts.skipped;  // a catalog outage or an empty catalog cache answered
    }
    return;
  }
  check(executable != nullptr, label + ": the policy refuses calls TypeScript sent to Alchemy");
  if (!executable) return;
  compare_body(wallet[0], RobinhoodRpcEnvelope(arena, "wallet_prepareCalls",
                                               RobinhoodPrepareParams(arena, from_lower, executable, "fixture-gas-policy")),
               label, counts);
  const Value* body = reply_of(arena, record, wallet[0]);
  if (!body) {
    check(status == 500 || status == 429, label + ": a failed wallet request is not TypeScript's 500/429");
    return;
  }
  Value* prepared = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  Value* requests = nullptr;
  size_t nodes = 0;
  const bool valid = rh::ReadRpcReply(arena, body, "wallet_prepareCalls", &prepared, &message, &data) ==
                         rh::RpcReply::kResult &&
                     rh::ValidatePrepared(arena, prepared, from_lower->text, &requests, &nodes);
  if (!valid) {
    check(status == 500, label + ": an invalid prepared operation is not TypeScript's 500");
    return;
  }
  if (status == 503) {  // the 1,000-ticket cap, after Alchemy
    check(text(tj::Get(record, "group")) == "cap", label + ": a 503 outside the cap group");
    return;
  }
  check(status == 200, label + ": a valid prepared operation is not TypeScript's 200");
  const Value* at = tj::Get(record, "at");
  char expires[32];
  const std::string ticket = ticket_for(static_cast<long long>(tj::Get(record, "ticketCounter")->number));
  check(fortis::codec::provider_text::IsoString(static_cast<int64_t>(at->number) + rh::TicketStore::kTtlMs, expires),
        label + ": expiresAt");
  compare_answer(record, RobinhoodPrepareResult(arena, ticket.c_str(), expires, prepared, requests), label, counts);
}

void submit_record(tj::Arena& arena, Context& context, const Value* record, const Value* input, Counts& counts) {
  const std::string label = "R44 " + text(tj::Get(record, "name"));
  const Value* env = context.groups[text(tj::Get(record, "group"))];
  const int status = status_of(record);
  if (!configured(env, "ALCHEMY_API_KEY")) {
    ++counts.unconfigured;
    check(status == 503 && text(tj::Get(record, "responseText")) == "{\"error\":\"SPONSORED_TRADES_UNAVAILABLE\"}",
          label + ": unconfigured submit is not TypeScript's 503");
    return;
  }
  const auto sends = calls_to(record, "wallet_sendPreparedCalls");
  const std::string ticket = text(tj::Get(input, "ticket"));
  const Value* signatures = tj::Get(input, "signatures");
  const auto stored = context.prepared_by_ticket.find(ticket);
  if (sends.empty()) {
    ++counts.skipped;  // 404, the count 400 or a lane refusal: the replay checks these
    check(status != 200, label + ": a 200 without a send");
    if (status == 400) {
      // The count refusal: the stored operation's node count differs.
      check(stored != context.prepared_by_ticket.end(), label + ": a count refusal for an unknown ticket");
      if (stored != context.prepared_by_ticket.end()) {
        const Value* prepared = stored->second;
        const bool array = tj::StringEquals(tj::Get(prepared, "type"), "array");
        const uint32_t nodes = array ? tj::Get(prepared, "data")->count : 1;
        check(nodes != signatures->count, label + ": TypeScript refused a matching signature count");
      }
    }
    return;
  }
  check(stored != context.prepared_by_ticket.end(), label + ": a send for a ticket no prepare record issued");
  if (stored == context.prepared_by_ticket.end()) return;
  Value* signed_call = rh::AttachSignatures(arena, stored->second, signatures->items, signatures->count);
  Value* params = tj::Arr(arena);
  check(signed_call && params && tj::Push(arena, params, signed_call), label + ": attachSignatures");
  compare_body(sends[0], RobinhoodRpcEnvelope(arena, "wallet_sendPreparedCalls", params), label, counts);
  const Value* body = reply_of(arena, record, sends[0]);
  Value* result = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  if (!body || rh::ReadRpcReply(arena, body, "wallet_sendPreparedCalls", &result, &message, &data) !=
                   rh::RpcReply::kResult) {
    check(status == 500, label + ": a failed send is not TypeScript's 500");
    return;
  }
  check(status == 200, label + ": a delivered send is not TypeScript's 200");
  compare_answer(record, result, label, counts);
}

void diagnose_record(tj::Arena& arena, Context& context, const Value* record, const Value* input, bool sponsored,
                     Counts& counts) {
  const std::string label = "R45 " + text(tj::Get(record, "name"));
  const Value* env = context.groups[text(tj::Get(record, "group"))];
  const int status = status_of(record);
  if (!configured(env, "ALCHEMY_API_KEY")) {
    ++counts.unconfigured;
    check(status == 503 && text(tj::Get(record, "responseText")) == "{\"error\":\"TRANSACTION_DIAGNOSTICS_UNAVAILABLE\"}",
          label + ": unconfigured diagnose is not TypeScript's 503");
    return;
  }
  const auto simulate = calls_to(record, "eth_simulateV1");
  const auto trace = calls_to(record, "debug_traceCallMany");
  const Value* from = tj::Get(input, "from");
  const Value* calls = tj::Get(input, "calls");
  bool exhausted = false;
  const Value* executable = rh::ValidateTradeCalls(arena, calls, context.stocks, from->text, from->length, &exhausted);
  if (simulate.empty()) {
    if (status == 400 && text(tj::Get(record, "group")) != "outage") {
      ++counts.policy_refusals;
      check(!executable, label + ": the policy accepts calls TypeScript refused");
    } else {
      ++counts.skipped;  // catalog outage/cache, a lane refusal
    }
    return;
  }
  check(executable != nullptr, label + ": the policy refuses calls TypeScript simulated");
  if (!executable) return;
  rh::DiagnosticsRequest request;
  request.calls = calls;
  request.executable = executable;
  request.from = from->text;
  request.from_length = from->length;
  request.sponsored = sponsored;
  request.stocks = &context.stocks;
  request.now_ms = static_cast<int64_t>(tj::Get(record, "at")->number);
  Value* simulation_calls = rh::SimulationCalls(arena, request);
  compare_body(simulate[0], RobinhoodRpcEnvelope(arena, "eth_simulateV1",
                                                 RobinhoodSimulateParams(arena, simulation_calls, sponsored)),
               label, counts);
  if (!trace.empty())
    compare_body(trace[0], RobinhoodRpcEnvelope(arena, "debug_traceCallMany", RobinhoodTraceParams(arena, simulation_calls)),
                 label + " (trace)", counts);
  const Value* body = reply_of(arena, record, simulate[0]);
  Value* simulation = nullptr;
  Value* message = nullptr;
  const Value* data = nullptr;
  const rh::RpcReply reply = body ? rh::ReadRpcReply(arena, body, "eth_simulateV1", &simulation, &message, &data)
                                  : rh::RpcReply::kInternal;
  if (reply == rh::RpcReply::kInternal) {
    check(status == 500 || status == 429, label + ": a failed simulation is not TypeScript's 500/429");
    return;
  }
  const Value* report = nullptr;
  if (reply == rh::RpcReply::kRpcError) {
    check(trace.empty(), label + ": TypeScript traced after a simulation error");
    const Value* reason = rh::DecodeRevertReason(arena, data);
    if (!reason && !cc::Blank(message->text, message->length)) reason = message;
    report = rh::FailedReport(arena, request, reason);
  } else {
    const Value* traced = nullptr;
    if (!trace.empty()) {
      const Value* trace_body = reply_of(arena, record, trace[0]);
      Value* result = nullptr;
      if (trace_body && rh::ReadRpcReply(arena, trace_body, "debug_traceCallMany", &result, &message, &data) ==
                            rh::RpcReply::kResult &&
          result->type != tj::Type::kNull)
        traced = result;
    }
    report = rh::BuildReport(arena, request, simulation, traced);
  }
  if (!report) {
    // The native 640-bit bound (named beside the handler): TypeScript 200.
    check(text(tj::Get(record, "name")) == "valid-fee-640-bit", label + ": the native report failed");
    ++counts.skipped;
    return;
  }
  check(status == 200, label + ": a report TypeScript did not answer with 200");
  compare_answer(record, report, label, counts);
}

void run_fixture(const char* path, RobinhoodWriteKind kind, const char* row) {
  const std::vector<uint8_t> bytes = read_file(path);
  check(!bytes.empty(), std::string("read ") + path);
  if (bytes.empty()) return;
  tj::Arena arena = tj::GrowingArena(size_t{1} << 30);
  const Value* fixture = tj::Parse(arena, bytes.data(), bytes.size());
  const char* fixture_name = strrchr(path, '/');
  if (!strict::ContractExpectations(arena, fixture, fixture_name ? fixture_name+1 : path)) return;
  check(fixture != nullptr, std::string("parse ") + path);
  if (!fixture) return;
  Context context;
  context.fixture = fixture;
  context.stocks = active_stocks(arena, fixture);
  check(context.stocks.count == 3, std::string(row) + ": three active catalog rows (TSLA, AAPL, TSLA.OLD)");
  const Value* groups = tj::Get(fixture, "groups");
  for (uint32_t i = 0; i < groups->count; ++i)
    context.groups[text(tj::Get(groups->items[i], "name"))] = tj::Get(groups->items[i], "env");
  Counts counts;
  const Value* cases = tj::Get(fixture, "cases");
  for (uint32_t i = 0; i < cases->count; ++i) {
    const Value* record = cases->items[i];
    const std::string route = text(tj::Get(record, "target"));
    tj::Arena scratch = tj::GrowingArena(size_t{64} << 20);
    // Every 200 JSON answer: round trip and the Struct encoder.
    auto encoded = [&](const Value* response) {
      if (status_of(response) != 200 || !tj::Get(response, "responseText") ||
          text(tj::Get(tj::Get(response, "headers"), "content-type")).rfind("application/json", 0) != 0)
        return;
      const std::string want = text(tj::Get(response, "responseText"));
      const Value* parsed = tj::Parse(scratch, reinterpret_cast<const uint8_t*>(want.data()), want.size());
      ++counts.round_trips;
      check(parsed && stringify(parsed) == want, std::string(row) + " " + text(tj::Get(record, "name")) +
                                                     ": JSON answer does not round-trip");
      if (const Value* proto = tj::Get(response, "protoOfJson")) {
        ++counts.encoded;
        check(encode_dynamic(parsed) == text(proto),
              std::string(row) + " " + text(tj::Get(record, "name")) + ": Struct re-encoding differs");
      }
    };
    if (tj::Get(record, "concurrent")) {
      const Value* responses = tj::Get(record, "responses");
      for (uint32_t r = 0; r < responses->count; ++r) encoded(responses->items[r]);
      ++counts.skipped;
      tj::FreeArena(scratch);
      continue;
    }
    encoded(record);
    // Prepare answers in any fixture are the stored operations of later submits.
    if (route.find("/sponsored-trades/prepare") != std::string::npos && status_of(record) == 200 &&
        tj::Get(record, "responseText")) {
      const std::string answer = text(tj::Get(record, "responseText"));
      const Value* parsed = tj::Parse(arena, reinterpret_cast<const uint8_t*>(answer.data()), answer.size());
      if (parsed) context.prepared_by_ticket[text(tj::Get(parsed, "ticket"))] = tj::Get(parsed, "prepared");
    }
    const bool own_route = kind == RobinhoodWriteKind::kPrepare ? route.find("/prepare") != std::string::npos
                           : kind == RobinhoodWriteKind::kSubmit ? route.find("/submit") != std::string::npos
                                                                 : route.find("/diagnose") != std::string::npos;
    if (!own_route || before_validation(record) || route.find('?') != std::string::npos ||
        text(tj::Get(record, "method")) != "POST" || route.back() == '/' ||
        route.find("/V1/") != std::string::npos) {
      // A submit fixture's setup prepares are R43 records already covered.
      ++counts.skipped;
      tj::FreeArena(scratch);
      continue;
    }
    ++counts.handled;
    const Value* input = input_of(scratch, record, kind);
    bool sponsored = false;
    const bool valid = RobinhoodValidWriteRequest(kind, input, &sponsored);
    const std::string label = std::string(row) + " " + text(tj::Get(record, "name"));
    if (strict::ProtoReject(row, text(tj::Get(record, "name")).c_str())) {
      check(!valid, label + ": strict protobuf request refusal");
      ++counts.zod_refusals;
    } else if (!valid) {
      ++counts.zod_refusals;
      check(status_of(record) == 400 && text(tj::Get(record, "responseText")) == "{\"error\":\"BAD_REQUEST\"}" &&
                tj::Get(record, "upstream")->count == 0,
            label + ": native zod refuses a request TypeScript accepted");
    } else if (kind == RobinhoodWriteKind::kPrepare) {
      prepare_record(scratch, context, record, input, counts);
    } else if (kind == RobinhoodWriteKind::kSubmit) {
      submit_record(scratch, context, record, input, counts);
    } else {
      diagnose_record(scratch, context, record, input, sponsored, counts);
    }
    tj::FreeArena(scratch);
  }
  printf("%s %s: %d handled (%d zod refusals, %d unconfigured, %d policy refusals), %d Alchemy bodies, "
         "%d rebuilt answers, %d round trips, %d Struct encodings, %d skipped\n",
         row, path, counts.handled, counts.zod_refusals, counts.unconfigured, counts.policy_refusals,
         counts.upstream_bodies, counts.answers, counts.round_trips, counts.encoded, counts.skipped);
  tj::FreeArena(arena);
}

// ---- the ticket store ------------------------------------------------------------------------------------

const char* kTicket = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
const char* kDevice = "01ARZ3NDEKTSV4RRFFQ69G5FAV";
const char* kOther = "01ARZ3NDEKTSV4RRFFQ69G5FB2";

struct ClaimRace {
  rh::TicketStore* store;
  int claimed = 0;
  pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
};

void* claim_thread(void* opaque) {
  auto* race = static_cast<ClaimRace*>(opaque);
  rh::TicketStore::Claim claim;
  if (race->store->Begin(kTicket, kDevice, 1000, 1, &claim) == rh::TicketStore::kClaimed) {
    pthread_mutex_lock(&race->mutex);
    ++race->claimed;
    pthread_mutex_unlock(&race->mutex);
    claim.prepared.reset();
  }
  return nullptr;
}

struct InsertRace {
  rh::TicketStore* store;
  int base;
  int inserted = 0;
};

void* insert_thread(void* opaque) {
  auto* race = static_cast<InsertRace*>(opaque);
  for (int i = 0; i < 130; ++i) {
    char ticket[33];
    snprintf(ticket, sizeof(ticket), "T%031d", race->base * 1000 + i);
    if (race->store->Insert(ticket, kDevice, 200000, "{}", 2, 1, 1000) == rh::TicketStore::kInserted) ++race->inserted;
  }
  return nullptr;
}

void ticket_store() {
  using TS = rh::TicketStore;
  const char* op = "{\"type\":\"user-operation-v070\"}";
  {
    TS store;
    TS::Claim claim;
    check(store.Insert(kTicket, kDevice, 121000, op, strlen(op), 1, 1000) == TS::kInserted, "store: insert");
    check(store.Begin(kTicket, kOther, 2000, 1, &claim) == TS::kNotFound, "store: another device is not found");
    check(store.Begin(kTicket, kDevice, 2000, 2, &claim) == TS::kWrongCount, "store: a wrong count is refused");
    check(store.Begin(kTicket, kDevice, 2000, 1, &claim) == TS::kClaimed && claim.length == strlen(op) &&
              memcmp(claim.prepared.get(), op, claim.length) == 0,
          "store: the device claims its operation after the refusals");
    TS::Claim second;
    check(store.Begin(kTicket, kDevice, 2000, 1, &second) == TS::kNotFound, "store: an in-flight ticket is not found");
    store.Finish(claim, false);
    claim.prepared.reset();
    check(store.Begin(kTicket, kDevice, 120999, 1, &claim) == TS::kClaimed, "store: a failed send leaves it claimable");
    store.Finish(claim, true);
    claim.prepared.reset();
    check(store.Begin(kTicket, kDevice, 3000, 1, &claim) == TS::kNotFound, "store: a delivered ticket is single use");
    check(store.Pending(3000) == 0, "store: delivery deletes the ticket");
  }
  {
    TS store;
    TS::Claim claim;
    store.Insert(kTicket, kDevice, 121000, op, strlen(op), 1, 1000);
    check(store.Begin(kTicket, kDevice, 121000, 1, &claim) == TS::kNotFound, "store: expiresAt <= now is expired");
    check(store.Pending(121000) == 0, "store: the expired ticket was pruned");
  }
  {
    // Pruned while in flight: a failure does not resurrect it, and a new
    // ticket with the same text is a new generation.
    TS store;
    TS::Claim claim;
    store.Insert(kTicket, kDevice, 121000, op, strlen(op), 1, 1000);
    check(store.Begin(kTicket, kDevice, 120000, 1, &claim) == TS::kClaimed, "store: claim before expiry");
    check(store.Pending(130000) == 0, "store: an expired in-flight ticket is pruned");
    store.Finish(claim, false);
    check(store.Pending(130000) == 0, "store: a failed send does not resurrect a pruned ticket");
    store.Insert(kTicket, kDevice, 300000, op, strlen(op), 1, 200000);
    store.Finish(claim, true);
    claim.prepared.reset();
    check(store.Pending(200000) == 1, "store: an old claim does not delete the new generation");
  }
  {
    // Map.set on an existing ticket replaces it in place.
    TS store;
    TS::Claim claim;
    store.Insert(kTicket, kDevice, 121000, op, strlen(op), 1, 1000);
    store.Insert(kTicket, kOther, 121000, "[1,2]", 5, 2, 1000);
    check(store.Pending(1000) == 1, "store: a repeated ticket replaces");
    check(store.Begin(kTicket, kOther, 2000, 2, &claim) == TS::kClaimed && claim.length == 5,
          "store: the replacement is bound to its own device and count");
    claim.prepared.reset();
  }
  {
    // The 1,000 cap (in flight or not) and one expiry freeing one slot.
    TS store;
    char ticket[33];
    bool all = true;
    for (int i = 0; i < 1000; ++i) {
      snprintf(ticket, sizeof(ticket), "C%031d", i);
      all = all && store.Insert(ticket, kDevice, 121000 + i, op, strlen(op), 1, 1000 + i) == TS::kInserted;
    }
    check(all, "store: 1,000 tickets");
    check(store.Insert("D0000000000000000000000000000000", kDevice, 300000, op, strlen(op), 1, 120999) == TS::kFull,
          "store: the 1,001st is refused");
    check(store.Insert("D0000000000000000000000000000000", kDevice, 300000, op, strlen(op), 1, 121000) == TS::kInserted,
          "store: the first expiry frees one slot");
    check(store.Insert("D0000000000000000000000000000001", kDevice, 300000, op, strlen(op), 1, 121000) == TS::kFull,
          "store: and only one");
  }
  {
    // The native 16 MiB bound on stored operations.
    TS store;
    std::string big(4 * 1024 * 1024, 'x');
    int inserted = 0;
    for (int i = 0; i < 5; ++i) {
      char ticket[33];
      snprintf(ticket, sizeof(ticket), "B%031d", i);
      inserted += store.Insert(ticket, kDevice, 121000, big.data(), big.size(), 1, 1000) == TS::kInserted;
    }
    check(inserted == 4, "store: 16 MiB of stored operations at most", std::to_string(inserted));
    check(store.Insert("x", kDevice, 121000, "{}", 2, 1, 1000) == TS::kFull, "store: a malformed ticket is refused");
  }
  {
    TS store;
    store.Insert(kTicket, kDevice, 121000, op, strlen(op), 1, 1000);
    ClaimRace race{&store};
    pthread_t threads[16];
    for (pthread_t& thread : threads) pthread_create(&thread, nullptr, claim_thread, &race);
    for (pthread_t& thread : threads) pthread_join(thread, nullptr);
    check(race.claimed == 1, "store: 16 simultaneous claims of one ticket → exactly one", std::to_string(race.claimed));
  }
  {
    TS store;
    InsertRace races[8];
    pthread_t threads[8];
    for (int i = 0; i < 8; ++i) {
      races[i] = InsertRace{&store, i};
      pthread_create(&threads[i], nullptr, insert_thread, &races[i]);
    }
    int inserted = 0;
    for (int i = 0; i < 8; ++i) {
      pthread_join(threads[i], nullptr);
      inserted += races[i].inserted;
    }
    check(inserted == 1000 && store.Pending(1000) == 1000, "store: 8 × 130 concurrent inserts stop at 1,000",
          std::to_string(inserted));
  }
  printf("ticket store: expiry, device binding, count, single use, in flight, failure release, prune, replace, "
         "cap, byte bound, concurrent claims and inserts\n");
}

}  // namespace

int main(int argc, char** argv) try {
  if (argc != 4) {
    fprintf(stderr, "usage: %s robinhood_prepare_edges.json robinhood_submit_edges.json robinhood_diagnose_edges.json\n",
            argv[0]);
    return 2;
  }
  check(ticket_for(0).size() == 32, "ticket text: 32 base64url characters");
  run_fixture(argv[1], RobinhoodWriteKind::kPrepare, "R43");
  run_fixture(argv[2], RobinhoodWriteKind::kSubmit, "R44");
  run_fixture(argv[3], RobinhoodWriteKind::kDiagnose, "R45");
  ticket_store();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
} catch (...) {
  return fortis::test::ReportException();
}
