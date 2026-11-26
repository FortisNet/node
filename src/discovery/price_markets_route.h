#pragma once

#include "discovery_route_support.h"

// POST /v1/prices/markets: the port of the TypeScript
// TokenDiscoveryFacet.markets behind its route. Up to 100 coin ids are
// trimmed, lower-cased, de-duplicated and sorted; each coin's market entry is
// cached per coin under `mkt:usd:<id>` (TTL.tokenMarket: 60 s fresh, 15 min
// stale). Cold coins are fetched from CoinGecko /coins/markets; a coin whose
// fetch fails (or is held after a recent failure) is reported in `pending`
// rather than failing the request, and an unlisted coin is simply absent.
// The full contract, with the named differences, is the comment above
// `handle` in price_markets_route.cpp.

namespace fortis::discovery {

// The route's request and batch tables (about 0.8 MiB), allocated by the
// discovery service so a disabled image pays nothing. Register at most one.
PriceMarketsState* CreatePriceMarketsState();
void DestroyPriceMarketsState(PriceMarketsState* state);
bool RegisterPriceMarketsRoute(http::HttpRouter& router, DiscoveryRouteContext* context);

// ── Below the HTTP layer (shared with the unit test) ──

constexpr size_t kPriceMarketsMaxIds = 100;     // zod ids.max(100)
constexpr size_t kPriceMarketsBatchIds = 50;    // ids per provider call

// The TypeScript requestCost: ceil(ids.length / 10), at least 1, from the
// raw parsed body (the last `ids` member of a JSON object when it is an
// array); a protobuf Buffer, a form or any other body costs 1.
uint32_t PriceMarketsCost(const DiscoveryBody& body);

// zod `marketsBody` then the facet's normalization: each id trimmed (zod),
// trim().toLowerCase(), de-duplicated, sorted by UTF-16 code units. False is
// the bare 400. A protobuf body is read as protobuf-es 2.16 reads
// MarketBatchRequest (DiscoveryProtoReader). Decoded JSON strings go to
// `scratch`; the normalized ids to `arena` (NUL-separated), `ids[i]` an
// offset/length into it.
struct PriceMarketsId {
  uint32_t offset;
  uint16_t length;
};
bool PriceMarketsIds(const DiscoveryBody& body, uint8_t* scratch, size_t scratch_capacity,
                     char* arena, size_t arena_capacity, PriceMarketsId* ids, size_t* count);

// The provider path of one batch: /api/v3/coins/markets?vs_currency=usd&ids=
// <encodeURIComponent(ids.join(','))>&price_change_percentage=1h,24h,7d,14d,
// 30d,200d,1y&sparkline=true for the first ids that fit (at most
// kPriceMarketsBatchIds and the transport's path bound). Returns how many.
size_t PriceMarketsBatchPath(const char* const* ids, const size_t* lengths, size_t count,
                             char* path, size_t capacity);

// The 200: `{"markets":[…],"pending":[…],"cache":{…}}` or MarketBatchResponse
// protobuf, written into the workspace output buffers. `markets[i]` is each
// served coin's stored market object text, in order.
struct PriceMarketsRenderInput {
  const uint8_t* const* markets = nullptr;
  const size_t* market_lengths = nullptr;
  size_t market_count = 0;
  const char* const* pending = nullptr;
  const size_t* pending_lengths = nullptr;
  size_t pending_count = 0;
  bool hit = false;
  bool partial = false;
  int64_t newest_ms = -1;
};
bool PriceMarketsRender(DiscoveryWorkspace* workspace, const PriceMarketsRenderInput& input,
                        bool protobuf, http::HttpResponse* response);

}  // namespace fortis::discovery
