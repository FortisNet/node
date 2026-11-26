#pragma once

#include "chain_tron_token.h"
#include "network_registry.h"
#include "token_facts.h"
#include "token_provider.h"
#include "token_ref.h"

#include <stddef.h>
#include <stdint.h>

// The TypeScript TronDriver.tokenSnapshot for the token routes:
// chainMeta (tokenMetadata), insight (tokenInsight) and holders
// (readTokenHolders, assigned only with a TronScan key) all read one
// TronTokenSnapshot for a TRC-20 contract or a numeric TRC-10 id:
//  1. with TRONSCAN_API_KEY: TronScan /api/token_trc20 or /api/token on the
//     'tronscan' lane, keyed; a 401/403/429 is re-issued once anonymously
//     (TronscanClient.get). A snapshot ends the read; null or any failure
//     falls through to the chain;
//  2. TronGrid on the 'trongrid' lane: /wallet/getassetissuebyid (TRC-10) or
//     four triggerconstantcontract views (TRC-20), never carrying holders.
// Every provider failure is null there (the driver catches them); only a
// cancelled read (deadline or shutdown) fails here, so no null is cached for
// a read that never finished.
//
// Concurrent readers of one {network, identifier} share a flight (the
// driver's FlightRegistry(100)): the first runs the provider stages and every
// joiner receives a copy of its snapshot. Flights hold no settled results.
// Guest-safe.
namespace fortis::tokens::token_tron {

struct Flights;
// Null when the process cannot allocate the registry (reads then run solo).
Flights* NewFlights();
void DeleteFlights(Flights* flights);

// tokenSnapshot answers this ref: a Tron-family address or TRC-10 id.
bool Snapshotted(const networks::TokenNetwork& network, const networks::TokenRef& ref);
// DriverTokenHolderSource.supports for Tron: the driver's tokenHolders exists
// only with a TronScan key.
bool HoldersSupported(const config::ProviderRuntimeConfig* config, const networks::TokenNetwork& network, const networks::TokenRef& ref);

enum class Outcome : uint8_t { kSnapshot, kNull, kFailed };

// Provider slots a snapshot stage may use after the caller's requests.
constexpr size_t kStageSlots = 4;

// tokenSnapshot(network, identifier). `batch` holds the caller's own
// `extra` provider requests and has room for kStageSlots more: those
// requests run in the snapshot's first provider batch (or, for a joiner,
// before it waits on the running flight); the caller interprets and releases
// them. *snapshot lives in the work arena.
Outcome Read(token_facts::Work& work, const networks::TokenNetwork& network, const networks::NativeNetworkConfig* seed,
             const char* identifier, providers::TokenFetch* batch, size_t extra, chains::tron_token::Snapshot* snapshot);

}  // namespace fortis::tokens::token_tron
