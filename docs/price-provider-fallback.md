# Spot price fallback and incorrect IDs

This document describes what the node does when its primary price provider, CoinGecko, cannot supply a spot price: which other providers it asks, in which order, under which limits, and how it decides that a fallback quote is usable. It also describes the public list of rejected coin IDs. Developers who work on `src/prices` or `src/providers`, and operators who investigate missing or stale prices, need it.

[kraken-price-fallback.md](kraken-price-fallback.md) describes the Kraken fallback in detail.

## Principles

- A spot request keeps its canonical ID, fiat currency, device authorization, quota, deadline and cancellation through every step. A fallback never extends the caller's deadline.
- Concurrent requests for the same IDs share one unit of provider work, called a flight. A flight moves to each fallback provider at most once.
- A fallback is tried after a CoinGecko failure, a quota response, a refusal by the CoinGecko lane or a successful answer that omits a requested USD quote.
- Assets are matched by explicit ID or by exact contract address. Ticker symbols are never matched.
- Only USD quotes fall back. The node does not derive other fiat currencies from a USD fallback.
- An asset that no provider covers stays unavailable. It is listed as missing and is never reported as zero.

## Fallback order

| Step | Provider | Covers |
| --- | --- | --- |
| 1 | CoinGecko | Primary source for coin IDs and contract addresses |
| 2 | Kraken, recent trades | `tron`, `avalanche-2` and `the-open-network` only |
| 3 | DefiLlama | Any canonical CoinGecko coin ID, through DefiLlama's `coingecko:ID` namespace, and ERC-20 contracts on mapped chains |
| 4 | MEXC, recent trades | Wiki Cat only (see below) |

Jupiter, Vestige, TronScan and Robinhood price paths are separate from CoinGecko and are not part of this chain.

For ERC-20 contracts DefiLlama is asked with an explicit chain mapping and the exact canonical address. The mapped chains are Ethereum, BSC, Arbitrum, Optimism, Polygon, Avalanche, Base, Cronos and XDC. Other chains have no DefiLlama fallback.

## DefiLlama

Requests go to `coins.llama.fi` over verified HTTPS without credentials ([provider API description](https://api-docs.defillama.com/llms-free.txt)).

| Limit | Value |
| --- | --- |
| IDs per request | 15 requested IDs; 16 provider keys including one internal conversion rate |
| Response size | 64 KiB |
| Transport timeout | 5 seconds, one attempt, within the original deadline |
| Lane | Two requests a second, two in flight, 32 queued |

The lane budget is the node's own conservative choice, not a limit stated by DefiLlama.

A quote is accepted only when it has the exact requested key, a finite positive price, a confidence of at least 0.9 and a provider timestamp that is at most five minutes old and not in the future. The node stores the provider's observation time and the source `defillama` with the quote and its price series.

A missing, stale or low-confidence fallback quote does not create a negative cache entry and is not evidence that the coin ID is wrong.

## Wiki Cat through MEXC

Wiki Cat has one additional fallback because its DefiLlama quote can be older than the five-minute limit; one probe found it about 28 minutes old. The MEXC market `WKCUSDT` is bound to the ID `wiki-cat` and to the exact BSC contract `0x6ec90334d89dbdc89e08a133271be3d104128edb`. No other ID, chain or ticker maps to this market.

- A DefiLlama request for this asset also asks for `coingecko:tether`, to learn the USDT/USD rate.
- If the Wiki Cat quote from DefiLlama is absent or stale and the USDT/USD rate is fresh, the flight makes one MEXC request: 8 KiB response limit, five-second timeout, lane of one request a second, one in flight, eight queued.
- The price is WKC/USDT × USDT/USD. Both observations must be at most five minutes old and not in the future. The result carries the older of the two timestamps and the source `mexc-defillama`.
- USDT is never assumed to equal one dollar.
- The USDT/USD rate is used only inside the flight. It is not added to the caller's result and is not stored as if it had been requested.

The identity of this market was checked against [CoinGecko's Wiki Cat page](https://www.coingecko.com/en/coins/wiki-cat), [MEXC's Wiki Cat page](https://www.mexc.com/price/WKC) and the [MEXC recent trades API](https://www.mexc.com/api-docs/spot-v3/market-data-endpoints/recent-trades-list).

## Background refresh

A warm thread refreshes the six core coins and up to six recently requested contract IDs every five seconds, when an entry is within 20 seconds of expiry. It submits this work to the same spot owner as foreground requests, so refresh uses the same lanes, fallbacks and deadlines. A CoinGecko cooldown does not stop refresh through the independent fallback lanes. A refresh never extends the timestamps of an old quote. The warm thread stops before the owner and the stores are destroyed.

## Incorrect coin IDs

The error page has a read-only section, `/errors#incorrect-coin-ids`. It lists two kinds of event:

- a price ID with invalid syntax;
- a `404` from CoinGecko for the profile lookup of one specific coin ID.

The second kind means that the provider rejected that exact lookup. It needs investigation; it does not prove that the asset does not exist. Lists, contract routes, history requests, query values and malformed identifiers are not counted as evidence about one coin ID. HTTP 429, 5xx, transport failures and empty batch results are reported separately.

Only IDs from the node's public catalog are shown by name. Other rejected identifiers are counted without their text, because a mistyped request could contain a wallet address or a credential. Counts and first and last times survive a restart. There is no endpoint to correct an ID.

## Tests

- `tests/defillama_price_test.cpp`, `tests/kraken_price_test.cpp` and `tests/mexc_price_test.cpp` cover request bounds, malformed, duplicate and oversized replies, missing and low-confidence quotes, timestamp limits, unsupported identities and the separation of credentials.
- `tests/spot_provider_owner_test.py` and `tests/price_fallback_warmer_test.py` run the owner against local HTTPS fixtures with a generated CA. They include a provider `429` followed by the CoinGecko lane's cooldown, during which the fallback must still answer.

An HTTP 200 from the price route is not proof of coverage. Tests and probes check the returned IDs, the `missing` list and freshness.
