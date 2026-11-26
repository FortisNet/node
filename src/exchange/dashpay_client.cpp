#include "dashpay_client.h"
#include "ownership.h"

#include "chain_transport.h"
#include "threading.h"
#include "token_json.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

namespace fortis::exchange::dashpay {
namespace {

namespace tj = fortis::codec::tj;

const char* const kQuorumBase = "https://quorums.mainnet.networks.dash.org";
const char* const kQuorumLane = "dashpay-quorums";
const char* const kGetDocumentsPath = "/org.dash.platform.dapi.v0.Platform/getDocuments";
const char* const kGrpcWebType = "application/grpc-web+proto";
constexpr size_t kMaxQuorums = 256;
constexpr size_t kMaxNodes = 1024;
constexpr uint32_t kConnectMs = 15000;
constexpr uint32_t kLookupMs = 15000;
constexpr uint32_t kRequestMs = 12000;
constexpr size_t kMaxReplyBytes = 64 * 1024;
constexpr size_t kTrustedArenaBytes = 16 * 1024 * 1024;
// One growing-arena block (1 MiB) holds the reply copy (at most 64 KiB).
constexpr size_t kReplyArenaBytes = 2 * 1024 * 1024;
constexpr unsigned kAttempts = 3;  // retries: 2
constexpr int64_t kTimeToleranceMs = 31 * 60 * 1000;
constexpr uint64_t kHeightTolerance = 1;
constexpr uint64_t kBaseBanMs = 60000;
constexpr uint64_t kMaxBanMs = 3600000;
// e^n for the ban ladder (rs-dapi-client: base x e^ban_count).
constexpr double kBanFactors[] = {1.0, 2.718281828, 7.389056099, 20.08553692, 54.59815003};

int64_t wall_ms() {
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);
  return static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

timespec realtime_after(uint64_t ms) {
  timespec out{};
  clock_gettime(CLOCK_REALTIME, &out);
  out.tv_sec += static_cast<time_t>(ms / 1000);
  out.tv_nsec += static_cast<long>((ms % 1000) * 1000000);
  if (out.tv_nsec >= 1000000000L) { ++out.tv_sec; out.tv_nsec -= 1000000000L; }
  return out;
}

int hex_digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// hex::decode of exactly `length` bytes.
bool decode_hex(const char* text, size_t text_length, uint8_t* out, size_t length) {
  if (text_length != length * 2) return false;
  for (size_t i = 0; i < length; ++i) {
    const int high = hex_digit(text[2 * i]), low = hex_digit(text[2 * i + 1]);
    if (high < 0 || low < 0) return false;
    out[i] = static_cast<uint8_t>(high << 4 | low);
  }
  return true;
}

// A dotted IPv4 literal (the guest network stack is IPv4-only).
bool ipv4_literal(const char* text, size_t length) {
  size_t i = 0;
  for (int octet = 0; octet < 4; ++octet) {
    if (octet) {
      if (i >= length || text[i] != '.') return false;
      ++i;
    }
    unsigned value = 0;
    size_t digits = 0;
    while (i < length && text[i] >= '0' && text[i] <= '9' && digits < 3) {
      value = value * 10 + static_cast<unsigned>(text[i++] - '0');
      ++digits;
    }
    if (!digits || value > 255 || (digits > 1 && text[i - digits] == '0')) return false;
  }
  return i == length;
}

struct Quorum {
  uint8_t hash[kHashBytes];
  uint8_t key[kPublicKeyBytes];
  bool key_ok;
};

struct Node {
  char url[40];   // https://255.255.255.255:65535
  char lane[48];  // dapi:255.255.255.255:65535
  uint64_t banned_until;
  uint32_t bans;
};

struct Trusted {
  std::unique_ptr<Quorum[], base::StorageDeleter> quorums;
  size_t quorum_count = 0;
  std::unique_ptr<Node[], base::StorageDeleter> nodes;
  size_t node_count = 0;
};

// QuorumsResponse.data[] / PreviousQuorumsResponse.data.quorums[]: entries
// with a malformed quorum hash are skipped (the provider's "Skipping invalid
// quorum hash"); the key is parsed on use, so a malformed key only fails a
// lookup that needs it. Current quorums come first and win.
bool add_quorums(const tj::Value* rows, Trusted* out) {
  if (!tj::IsArray(rows)) return false;
  for (size_t i = 0; i < rows->count; ++i) {
    const tj::Value* row = rows->items[i];
    const tj::Value* hash = tj::IsObject(row) ? tj::Get(row, "quorum_hash") : nullptr;
    const tj::Value* key = tj::IsObject(row) ? tj::Get(row, "key") : nullptr;
    if (!tj::IsString(hash) || !tj::IsString(key)) return false;  // serde: required strings
    Quorum quorum{};
    if (!decode_hex(hash->text, hash->length, quorum.hash, kHashBytes)) continue;
    bool duplicate = false;
    for (size_t j = 0; j < out->quorum_count && !duplicate; ++j)
      duplicate = memcmp(out->quorums[j].hash, quorum.hash, kHashBytes) == 0;
    if (duplicate || out->quorum_count >= kMaxQuorums) continue;
    const char* text = key->text;
    size_t length = key->length;
    if (length >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) { text += 2; length -= 2; }
    quorum.key_ok = decode_hex(text, length, quorum.key, kPublicKeyBytes);
    out->quorums[out->quorum_count++] = quorum;
  }
  return true;
}

// MasternodeDiscoveryResponse: success must be true; ENABLED entries whose
// versionCheck is "success" become https://<host>:<platformHTTPPort|443>.
// Hosts that are not IPv4 literals are skipped (documented: the guest
// network stack is IPv4-only).
bool add_nodes(const tj::Value* document, Trusted* out) {
  const tj::Value* success = tj::IsObject(document) ? tj::Get(document, "success") : nullptr;
  const tj::Value* rows = tj::IsObject(document) ? tj::Get(document, "data") : nullptr;
  if (!success || success->type != tj::Type::kTrue || !tj::IsArray(rows)) return false;
  for (size_t i = 0; i < rows->count && out->node_count < kMaxNodes; ++i) {
    const tj::Value* row = rows->items[i];
    if (!tj::IsObject(row)) return false;
    const tj::Value* address = tj::Get(row, "address");
    const tj::Value* status = tj::Get(row, "status");
    const tj::Value* check = tj::Get(row, "versionCheck");
    const tj::Value* port = tj::Get(row, "platformHTTPPort");
    if (!tj::IsString(address) || !tj::IsString(status)) return false;
    if (!tj::StringEquals(status, "ENABLED") || !tj::IsString(check) || !tj::StringEquals(check, "success")) continue;
    // rsplit_once(':'): the host is everything before the last colon.
    size_t host_length = address->length;
    for (size_t k = address->length; k > 0; --k)
      if (address->text[k - 1] == ':') { host_length = k - 1; break; }
    if (!ipv4_literal(address->text, host_length)) continue;
    unsigned port_value = 443;
    if (port && port->type != tj::Type::kNull) {
      if (!tj::IsNumber(port) || port->number < 0 || port->number > 65535 || port->number != static_cast<unsigned>(port->number))
        return false;
      port_value = static_cast<unsigned>(port->number);
      if (!port_value) return false;
    }
    Node node{};
    snprintf(node.url, sizeof(node.url), "https://%.*s:%u", static_cast<int>(host_length), address->text, port_value);
    snprintf(node.lane, sizeof(node.lane), "dapi:%.*s:%u", static_cast<int>(host_length), address->text, port_value);
    out->nodes[out->node_count++] = node;
  }
  return out->node_count > 0;
}

}  // namespace

struct Client::Impl {
  chains::Transport* transport = nullptr;
  ClientSeams seams{};
  mutable fortis::base::Mutex mutex;
  fortis::base::ConditionVariable changed;
  bool ready = false;
  bool connecting = false;
  uint64_t generation = 0;
  Trusted trusted;
  uint64_t max_height = 0;
  uint64_t rng = 0;

  ~Impl() {
    trusted.quorums.reset();
    trusted.nodes.reset();


  }

  uint32_t ms(uint32_t seam, uint32_t fallback) const { return seam ? seam : fallback; }
  int64_t Wall() const { return seams.clock ? seams.clock->NowMs() : wall_ms(); }

  uint32_t Random() {
    if (seams.node_choice) return seams.node_choice->Next();
    // xorshift64 under the mutex (callers hold it).
    if (!rng) rng = chains::ChainMonotonicMs() * 0x9e3779b97f4a7c15ULL | 1;
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return static_cast<uint32_t>(rng >> 32);
  }

  // GETs the trusted lists (the nodes too when `nodes` is set) into `out`.
  bool Fetch(bool nodes, uint64_t deadline, Trusted* out, char* reason, size_t reason_size) {
    const char* const urls[] = {kQuorumBase};
    const char* const paths[] = {"/quorums", "/previous", "/masternodes"};
    chains::Call calls[3];
    const size_t count = nodes ? 3 : 2;
    const uint64_t now = chains::ChainMonotonicMs();
    if (deadline <= now) {
      snprintf(reason, reason_size, "connect deadline");
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      calls[i].kind = chains::CallKind::kGet;
      calls[i].urls = urls;
      calls[i].url_count = 1;
      calls[i].path = paths[i];
      calls[i].lane = kQuorumLane;
      calls[i].timeout_ms = static_cast<uint32_t>(deadline - now < 60000 ? deadline - now : 60000);
    }
    tj::Arena arena = tj::GrowingArena(kTrustedArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    transport->Run(calls, count, arena, deadline);
    bool ok = true;
    for (size_t i = 0; i < count && ok; ++i) {
      if (!calls[i].ok()) {
        snprintf(reason, reason_size, "trusted %s: %.48s", paths[i], calls[i].message);
        ok = false;
      }
    }
    if (ok) {
      const tj::Value* current = tj::IsObject(calls[0].value) ? tj::Get(calls[0].value, "data") : nullptr;
      const tj::Value* previous = tj::IsObject(calls[1].value) ? tj::Get(calls[1].value, "data") : nullptr;
      const tj::Value* previous_rows = tj::IsObject(previous) ? tj::Get(previous, "quorums") : nullptr;
      ok = add_quorums(current, out) && add_quorums(previous_rows, out);
      if (!ok) snprintf(reason, reason_size, "trusted quorum list malformed");
      if (ok && nodes && !add_nodes(calls[2].value, out)) {
        snprintf(reason, reason_size, "no eligible masternode addresses");
        ok = false;
      }
    }
    tj::FreeArena(arena);
    return ok;
  }

  bool Allocate(Trusted* t) {
    t->quorums = base::AllocateZeroedStorage<Quorum[]>(kMaxQuorums, sizeof(Quorum));
    t->nodes = base::AllocateZeroedStorage<Node[]>(kMaxNodes, sizeof(Node));
    t->quorum_count = t->node_count = 0;
    return t->quorums && t->nodes;
  }

  // The connection for this lookup: the current one, or a new one.
  bool Ensure(uint64_t deadline, uint64_t* generation_out, char* reason, size_t reason_size) {
    fortis::base::UniqueLock lock(mutex);
    while (!ready && connecting) {
      const uint64_t now = chains::ChainMonotonicMs();
      if (now >= deadline) break;
      const timespec wake = realtime_after(deadline - now < 200 ? deadline - now : 200);
      changed.WaitUntilLocked(mutex, wake);
    }
    if (ready) {
      *generation_out = generation;
      return true;
    }
    if (connecting) {
      lock.Unlock();
      snprintf(reason, reason_size, "connect deadline");
      return false;
    }
    connecting = true;
    lock.Unlock();
    const uint64_t now = chains::ChainMonotonicMs();
    const uint64_t connect_deadline = now + ms(seams.connect_ms, kConnectMs) < deadline
                                          ? now + ms(seams.connect_ms, kConnectMs)
                                          : deadline;
    Trusted fresh;
    const bool ok = Allocate(&fresh) && Fetch(true, connect_deadline, &fresh, reason, reason_size);
    lock.Lock();
    connecting = false;
    if (ok) {
      trusted.quorums.reset();
      trusted.nodes.reset();
      trusted = std::move(fresh);
      ready = true;
      max_height = 0;
      *generation_out = ++generation;
    } else {
      fresh.quorums.reset();
      fresh.nodes.reset();
    }
    changed.Broadcast();
    return ok;
  }

  // Refetches the quorum lists into the current generation (a quorum the
  // proof names is not cached yet). Known quorums are kept.
  bool RefreshQuorums(uint64_t gen, uint64_t deadline) {
    Trusted fresh;
    fresh.quorums = base::AllocateZeroedStorage<Quorum[]>(kMaxQuorums, sizeof(Quorum));
    char reason[96];
    const bool ok = fresh.quorums && Fetch(false, deadline, &fresh, reason, sizeof(reason));
    bool merged = false;
    if (ok) {
      fortis::base::LockGuard lock(mutex);
      if (ready && generation == gen) {
        for (size_t i = 0; i < fresh.quorum_count; ++i) {
          bool known = false;
          for (size_t j = 0; j < trusted.quorum_count && !known; ++j)
            known = memcmp(trusted.quorums[j].hash, fresh.quorums[i].hash, kHashBytes) == 0;
          if (!known && trusted.quorum_count < kMaxQuorums) trusted.quorums[trusted.quorum_count++] = fresh.quorums[i];
        }
        merged = true;
      }
    }
    fresh.quorums.reset();
    return merged;
  }

  bool QuorumKey(uint64_t gen, const uint8_t hash[kHashBytes], uint8_t key[kPublicKeyBytes]) {
    fortis::base::LockGuard lock(mutex);
    bool found = false;
    if (generation == gen) {
      for (size_t i = 0; i < trusted.quorum_count && !found; ++i) {
        if (memcmp(trusted.quorums[i].hash, hash, kHashBytes) != 0) continue;
        found = trusted.quorums[i].key_ok;
        if (found) memcpy(key, trusted.quorums[i].key, kPublicKeyBytes);
        break;
      }
    }
    return found;
  }

  bool Pick(uint64_t gen, Node* out) {
    fortis::base::LockGuard lock(mutex);
    const uint64_t now = chains::ChainMonotonicMs();
    size_t live = 0;
    if (ready && generation == gen)
      for (size_t i = 0; i < trusted.node_count; ++i) live += trusted.nodes[i].banned_until <= now;
    bool ok = live > 0;
    if (ok) {
      size_t pick = Random() % live;
      for (size_t i = 0; i < trusted.node_count; ++i) {
        if (trusted.nodes[i].banned_until > now) continue;
        if (pick-- == 0) { *out = trusted.nodes[i]; break; }
      }
    }
    return ok;
  }

  Node* FindNode(const Node& node) {
    for (size_t i = 0; i < trusted.node_count; ++i)
      if (strcmp(trusted.nodes[i].url, node.url) == 0) return &trusted.nodes[i];
    return nullptr;
  }

  void Ban(uint64_t gen, const Node& node) {
    fortis::base::LockGuard lock(mutex);
    Node* entry = generation == gen ? FindNode(node) : nullptr;
    if (entry) {
      const size_t step = entry->bans < 4 ? entry->bans : 4;
      double period = static_cast<double>(kBaseBanMs) * kBanFactors[step];
      if (entry->bans > 4 || period > static_cast<double>(kMaxBanMs)) period = static_cast<double>(kMaxBanMs);
      entry->banned_until = chains::ChainMonotonicMs() + static_cast<uint64_t>(period);
      ++entry->bans;
    }
  }

  void Unban(uint64_t gen, const Node& node) {
    fortis::base::LockGuard lock(mutex);
    Node* entry = generation == gen ? FindNode(node) : nullptr;
    if (entry) {
      entry->bans = 0;
      entry->banned_until = 0;
    }
  }

  // Sdk::verify_response_metadata: the time window first, then the
  // monotonic height window (which also raises the high-water mark).
  bool Fresh(uint64_t gen, const DocumentsProof& proof) {
    const int64_t now = Wall();
    const uint64_t time = proof.time_ms;
    const uint64_t local = now > 0 ? static_cast<uint64_t>(now) : 0;
    const uint64_t skew = time > local ? time - local : local - time;
    if (skew > static_cast<uint64_t>(kTimeToleranceMs)) return false;
    fortis::base::LockGuard lock(mutex);
    bool fresh = generation == gen;
    if (fresh) {
      if (proof.height > max_height) max_height = proof.height;
      const uint64_t expected = max_height;
      fresh = !(expected > kHeightTolerance && proof.height < expected - kHeightTolerance);
    }
    return fresh;
  }

  void Invalidate(uint64_t gen) {
    fortis::base::LockGuard lock(mutex);
    if (ready && generation == gen) ready = false;
  }
};

namespace {

// The trusted quorum keys of one connection generation.
class ConnectionQuorumKeys final : public QuorumKeys {
 public:
  ConnectionQuorumKeys(Client::Impl* impl, uint64_t generation) : impl_(impl), generation_(generation) {}
  bool PublicKey(uint32_t, const uint8_t hash[kHashBytes], uint8_t key[kPublicKeyBytes]) override {
    return impl_->QuorumKey(generation_, hash, key);
  }

 private:
  Client::Impl* impl_;
  uint64_t generation_;
};

}  // namespace

void Client::ImplDeleter::operator()(Impl* impl) const { fortis::base::Delete(impl); }

Client::~Client() { Stop(); }

bool Client::Start(chains::Transport* transport, const ClientSeams* seams) {
  if (impl_ || !transport) return false;
  impl_.reset(fortis::base::New<Impl>());
  if (!impl_) return false;
  impl_->transport = transport;
  if (seams) impl_->seams = *seams;
  return true;
}

void Client::Stop() {
  impl_.reset();
}

uint64_t Client::generation() const {
  if (!impl_) return 0;
  fortis::base::LockGuard lock(impl_->mutex);
  const uint64_t value = impl_->ready ? impl_->generation : 0;
  return value;
}

LookupResult Client::Resolve(const char* label, size_t length, uint64_t deadline_ms) {
  LookupResult result;
  if (!impl_) {
    snprintf(result.reason, sizeof(result.reason), "not started");
    return result;
  }
  uint8_t frame[256];
  const size_t frame_length = BuildGetDocumentsRequest(label, length, frame, sizeof(frame));
  if (!frame_length) {
    snprintf(result.reason, sizeof(result.reason), "invalid label");
    return result;
  }
  uint64_t gen = 0;
  if (!impl_->Ensure(deadline_ms, &gen, result.reason, sizeof(result.reason))) return result;
  const uint64_t start = chains::ChainMonotonicMs();
  const uint64_t lookup_ms = impl_->ms(impl_->seams.lookup_ms, kLookupMs);
  const uint64_t deadline = start + lookup_ms < deadline_ms ? start + lookup_ms : deadline_ms;
  ConnectionQuorumKeys quorum(impl_.get(), gen);
  bool refreshed = false;
  for (unsigned attempt = 0; attempt < kAttempts; ++attempt) {
    const uint64_t now = chains::ChainMonotonicMs();
    if (now >= deadline) {
      snprintf(result.reason, sizeof(result.reason), "lookup deadline");
      break;
    }
    Node node{};
    if (!impl_->Pick(gen, &node)) {
      snprintf(result.reason, sizeof(result.reason), "no live evonode");
      break;
    }
    const char* const urls[] = {node.url};
    chains::Call call;
    call.kind = chains::CallKind::kRawPost;
    call.urls = urls;
    call.url_count = 1;
    call.path = kGetDocumentsPath;
    call.lane = node.lane;
    call.raw_body = frame;
    call.raw_body_length = frame_length;
    call.content_type = kGrpcWebType;
    call.headers[0] = {"accept", kGrpcWebType};
    call.headers[1] = {"x-grpc-web", "1"};
    call.header_count = 2;
    call.max_response_bytes = kMaxReplyBytes;
    const uint64_t request_ms = impl_->ms(impl_->seams.request_ms, kRequestMs);
    call.timeout_ms = static_cast<uint32_t>(deadline - now < request_ms ? deadline - now : request_ms);
    tj::Arena arena = tj::GrowingArena(kReplyArenaBytes);
    std::unique_ptr<tj::Arena, tj::ArenaBlocksDeleter> arena_blocks(&arena);
    impl_->transport->Run(&call, 1, arena, deadline);
    Failure failure = Failure::kNone;
    DocumentsProof proof;
    Resolution resolution;
    if (!call.ok()) {
      snprintf(result.reason, sizeof(result.reason), "evonode: %.64s", call.message);
    } else {
      failure = ParseGrpcWebDocumentsResponse(call.raw_response, call.raw_response_length, &proof);
      if (failure == Failure::kNone)
        failure = VerifyDpnsName(label, length, proof, &quorum, &resolution);
      if (failure == Failure::kQuorumUnknown && !refreshed) {
        refreshed = true;
        if (impl_->RefreshQuorums(gen, deadline))
          failure = VerifyDpnsName(label, length, proof, &quorum, &resolution);
      }
      if (failure != Failure::kNone) snprintf(result.reason, sizeof(result.reason), "proof: %s", FailureName(failure));
      else if (!impl_->Fresh(gen, proof)) {
        failure = Failure::kCommit;
        snprintf(result.reason, sizeof(result.reason), "stale node metadata");
      }
    }
    tj::FreeArena(arena);
    if (!call.ok() || failure != Failure::kNone) {
      impl_->Ban(gen, node);
      continue;
    }
    impl_->Unban(gen, node);
    result.outcome = resolution.found ? Outcome::kFound : Outcome::kAbsent;
    if (resolution.found) memcpy(result.identity, resolution.owner_id, kHashBytes);
    result.reason[0] = '\0';
    return result;
  }
  // Ended unverified: drop this connection (only if it is still current).
  impl_->Invalidate(gen);
  result.outcome = Outcome::kUnavailable;
  return result;
}

}  // namespace fortis::exchange::dashpay
