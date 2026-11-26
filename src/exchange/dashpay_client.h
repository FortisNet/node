#pragma once

#include <memory>
#include "clock.h"
#include "ownership.h"
#include "dashpay_proof.h"

#include <stddef.h>
#include <stdint.h>

namespace fortis::chains { class Transport; }

// The trusted Dash Platform client behind GET /v1/dashpay/names/resolve: the
// native counterpart of the TypeScript EvoDashPayNameResolver
// over EvoSDK.mainnetTrusted().
//
// A connection is the trusted context the Evo SDK prefetches: the quorum
// public keys from https://quorums.mainnet.networks.dash.org/quorums and
// /previous (the only trust anchor — a miss refetches both, as
// rs-sdk-trusted-context-provider does) and the DAPI evonodes from
// /masternodes (ENABLED, versionCheck "success", https://<ipv4>:<platform
// HTTP port>). A lookup sends the gRPC-web getDocuments request to a random
// live evonode, verifies the reply with dashpay_proof.h, then applies the
// SDK's freshness rules (signed time within 31 minutes of the local clock;
// height at most one block below the highest this connection has seen).
// Proof, freshness and transport failures retry on another evonode (three
// attempts, the failed address banned for 60 s × e^bans, as rs-dapi-client).
//
// A verified miss keeps the connection; a lookup that ends unverified drops
// it, but only the generation it used — an old failure never clears a newer
// healthy connection. Connecting and looking up are each bounded by 15 s
// (the TypeScript deadline()s) inside the caller's overall deadline.
//
// All requests run on the shared provider coordinator through
// chains::Transport (lanes "dashpay-quorums" and "dapi:<ip>"), with the
// validated CA bundle; evonodes present publicly trusted IP certificates.
namespace fortis::exchange::dashpay {

// The random number behind the evonode choice. The base always answers 0:
// the first live node in the trusted list.
class NodeChoice {
 public:
  virtual uint32_t Next() { return 0; }

 protected:
  constexpr NodeChoice() = default;
  ~NodeChoice() = default;
};

struct ClientSeams {
  // The wall clock (NowMs) for the freshness check.
  fortis::base::Clock* clock = nullptr;
  // Evonode choice; null uses a xorshift seeded from the monotonic clock
  // (the choice is not a security boundary: every reply is verified).
  NodeChoice* node_choice = nullptr;
  // Replace the 15 s connect / 15 s lookup / 12 s request bounds.
  uint32_t connect_ms = 0;
  uint32_t lookup_ms = 0;
  uint32_t request_ms = 0;
};

enum class Outcome : uint8_t { kFound, kAbsent, kUnavailable };

struct LookupResult {
  Outcome outcome = Outcome::kUnavailable;
  uint8_t identity[kHashBytes]{};
  // A short diagnostic (no name or identity): the last failure.
  char reason[96]{};
};

class Client {
 public:
  Client() = default;
  ~Client();
  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  // `transport` must be started and outlive the client.
  bool Start(fortis::chains::Transport* transport, const ClientSeams* seams = nullptr);
  void Stop();

  // Thread-safe. `label` is a normalized DPNS label (3..63 of [a-z0-9-]);
  // `deadline_ms` is monotonic and bounds connecting plus looking up.
  LookupResult Resolve(const char* label, size_t length, uint64_t deadline_ms);

  // Tests: the current connection generation (0 = none).
  uint64_t generation() const;

  struct Impl;

 private:
  // Defined beside Impl: only that unit can destroy one.
  struct ImplDeleter { void operator()(Impl* impl) const; };
  std::unique_ptr<Impl, ImplDeleter> impl_;
};

}  // namespace fortis::exchange::dashpay
