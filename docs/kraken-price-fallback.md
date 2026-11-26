# Kraken USD price fallback

This document describes the first spot price fallback: for three coins, the node can take a USD price from Kraken's public recent-trades endpoint when CoinGecko does not supply one. It states the mapping, the limits, the checks on a trade and how the result is labeled. Developers who work on `src/providers/kraken_price.cpp` and operators who see the source `kraken` in a price response need it.

[price-provider-fallback.md](price-provider-fallback.md) describes the whole fallback chain.

## Scope

CoinGecko is the primary spot price provider. After an unsuccessful CoinGecko attempt, or a successful response that omits a requested quote, exactly these IDs can use Kraken:

| Canonical node ID | Kraken market |
| --- | --- |
| `tron` | `TRXUSD` |
| `avalanche-2` | `AVAXUSD` |
| `the-open-network` | `TONUSD` |

The mapping is explicit. It is not derived from ticker symbols.

- No Kraken account, trading access or API key is used.
- Requests go to `api.kraken.com` over verified HTTPS, with the same public-address policy and CA bundle as all outbound requests. No CoinGecko header is sent to Kraken.
- Only USD spot quotes are covered. Other assets, other currencies, market rankings and OHLC history are not affected.

Provider API: [Kraken recent trades](https://docs.kraken.com/api-reference/market-data/get-recent-trades).

## Execution

- A flight (the shared provider work for one set of IDs) moves from CoinGecko to Kraken at most once. It keeps its waiting callers, cancellation, admission limits and original deadline.
- Quotes that CoinGecko did supply are kept. Kraken is asked only for the eligible IDs that are still missing.
- An ID that is eligible for fallback is not written to the cache as missing before the fallback has finished.
- A refusal by the CoinGecko lane or its circuit breaker also starts the fallback.
- Expiry of the original deadline, or shutdown, stops the work. When both providers fail, the normal cache policy decides the answer: a stale value if one exists, otherwise missing.

| Limit | Value |
| --- | --- |
| Lane | One request a second, burst one, one in flight, 20 queued, five-second queue timeout |
| Request | One market per request, one attempt, five-second transport timeout |
| Response size | 8 KiB |

## Checks on a trade

The node accepts a Kraken answer only when:

- the provider's error array is empty;
- the answer contains the exact requested market and one trade;
- the price is a positive finite decimal;
- the trade has a numeric timestamp that is not in the future and at most five minutes old.

It rejects duplicate required keys and malformed or oversized responses. A failed fallback never creates a negative cache entry. The node does not substitute zero, a stablecoin price or a relabeled old quote.

## Result

- The quote's `source` is `kraken`, in the first response and in later cache hits.
- The price is the latest accepted trade on one exchange. It is not a market-wide average.
- The trade's timestamp is stored as the provider observation time, next to the quote. The time of receipt and the normal 30-second cache freshness stay separate from it.
- The node reports no 24-hour change or volume for these quotes.
- JSON and protobuf formats and the stored quote format are unchanged.

## Tests

- `tests/kraken_price_test.cpp` covers the mappings, request bounds, a missing CA configuration, malformed, error and duplicate responses, invalid decimals, timestamp overflow, the age limits, future timestamps, input bounds and wrong markets.
- `tests/spot_provider_owner_test.py` runs the owner against local HTTPS fixtures with a generated CA: partial CoinGecko success, concurrent callers sharing one fallback, quota rejection, a malformed fallback answer without negative caching, a successful retry, stored source and timestamps, protobuf reads from a warm cache, an unsupported fiat currency, and cancellation and shutdown during a fallback request.

The release build runs the same HTTPS fixture against the release objects ([native-runtime.md](native-runtime.md#build)).
