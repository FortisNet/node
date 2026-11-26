# Node home page

This document describes the page a node serves at `/`: what it shows, how the list of services is derived, and how to edit and regenerate it. Developers who change the page or its assets, and operators who run a node under their own hostname, need it.

The home page introduces FortisNet and lists the node's interfaces. It links to the live dashboard at `/dashboard` and to request statistics at `/stats` ([node-dashboard.md](node-dashboard.md)). It works without JavaScript and makes no external request: no remote fonts, no CDN.

On a managed host the page at `/` can come from the host instead. If the directory named by `FORTIS_RELEASE_STATUS_DIR` contains `index.html`, the node serves that file unchanged and the built-in page described here is the fallback ([node-updates.md](node-updates.md)).

## Service list

The list is derived from the startup configuration. It is not a fixed claim.

| Displayed service | Startup switch |
| --- | --- |
| Price discovery, including token discovery | `FORTIS_NATIVE_PRICE_ROUTE_CANARY` |
| News and media | `FORTIS_NATIVE_NEWS_CANARY` |
| AI conversations | `FORTIS_NATIVE_AI_CANARY` |
| Network catalog | Always registered |
| Wallet and chain data | `FORTIS_NATIVE_ADDRESS_CANARY` |
| Cross-chain quotes | `FORTIS_NATIVE_CROSS_CHAIN_CANARY` |
| Tokenized stocks | `FORTIS_NATIVE_ROBINHOOD_CANARY` |
| Dash payments | `FORTIS_NATIVE_DASHPAY_CANARY` |

A check mark means that the interface is enabled. It does not mean that its providers are healthy or that its results are complete. A disabled service stays on the page, marked **Not enabled**. The page also explains that the API requires device authorization and depends on provider quotas.

Opening the page makes no provider call and reads no credential and no database row.

## Future features

[`contracts/node-future-features.json`](../contracts/node-future-features.json) is the list of possible future features: VPN, encrypted storage, compute, agent discovery and agent agreements. These are possibilities. They are not deployed, not promised for a version and have no date. The node serves the same JSON at `/node-features.json` and renders it into the home page at build time, with its explanatory notice.

## Editing and regenerating

Edit the JSON or [`assets/node-home.html`](../assets/node-home.html), then run:

```sh
make generate-node-home generate-api-docs
make check-api-docs
```

`make check-api-docs` fails when the generated files do not match their inputs, and runs the generator's tests for schema rejection and escaping. `make test` and `make verify` run the same check.

Limits:

- The feature list has 1 to 16 entries with unique URL-safe IDs and bounded text.
- The static HTML has a byte budget. The handler renders into a 32 KiB buffer, and the generator reserves at least 4 KiB of it for the service list.
- The renderer writes into storage that the caller owns and returns zero when the page does not fit. It never sends a truncated page.

## Images and previews

`assets/` holds `node-hall.webp`, `fortisnet-lockup.svg`, `fortis-favicon.svg`, `background.jpg` and `social-preview.jpg`. The favicon adds a contrasting background to the FortisNet mark so that it is visible on dark browser chrome. The social image is 1200 × 630 pixels.

The assets are compiled into the program unchanged. The node serves each with its image MIME type, one-day caching, `HEAD` support and the normal security headers. Only the registered asset paths are served.

The Open Graph and Twitter tags in `assets/node-home.html` contain a fixed canonical URL and absolute image URLs. They do not depend on the request's `Host` header. An operator who serves the node under another hostname must change these URLs in `assets/node-home.html` and regenerate before building a release.

## Verification

- `operations_pages_test` renders all 128 combinations of the seven service switches, with exact-fit, short and null buffers, and compares the embedded assets byte for byte.
- `tests/operations_routes_test.py` checks the public routes on the real program: methods, query handling, security headers, JSON, asset bytes, MIME types and `HEAD`.
- `tests/reference/dashboard_browser_test.mjs` checks the page in Chromium at desktop width and at 390 and 320 pixels, without JavaScript, including the feature list and the social tags.
