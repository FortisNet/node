# Namespaces

This document states the namespace rules of the C++ sources. Anyone who adds
or moves a declaration under `src/` needs it.

One rule: **every C++ declaration in `src/<module>/` lives in
`fortis::<module>`**, where `<module>` is the directory name (`fortis::base`,
`fortis::http`, `fortis::prices`, ...). `scripts/check_layers.py` enforces it.
Run it with `make check-layers`; `make test` runs it too.

## Inside a module

- A module may group a feature in a namespace nested in its own:
  `fortis::wallet::address`, `fortis::exchange::cross_chain`,
  `fortis::codec::tj`. The main nested namespaces, by module (the list is not
  exhaustive):

  | module    | nested namespaces |
  |-----------|-------------------|
  | base      | `clock_detail`, `log_internal`, `stdlib_check_detail` |
  | storage   | `kv_internal`, `kv_checksum_detail` |
  | http      | `query_string` |
  | codec     | `tj` (token JSON DOM), `history_batch` (writers), `proto_wire`, `json_lex`, `utf8`, `crash_report`, `sandbox` |
  | compat    | `url`, `idna`, `idna_tables`, `js_json`, `js_regex`, `js_case_tables`, `redaction`, `token_text`, `sandbox` |
  | chains    | `asset_index`, `tron_token`, `utxo_detail` |
  | discovery | `market_view`, `coin_popularity`, `coin_popularity_store` |
  | prices    | `coin_prices`, `history_batch` (history views), `history_query`, `ohlc_query`, `spot_query`, `spot_provider_owner_internal` |
  | news      | `json`, `news_routes_internal`, `news_read_rules` |
  | exchange  | `cross_chain` (+ `detail`), `dashpay`, `robinhood`, `robinhood_service_internal` |
  | tokens    | `token_facts`, `token_data`, `token_jobs`, `token_page`, ... (one per token facet), `comparison`, `price_jobs`, `saucerswap`, `token_service_internal` |
  | wallet    | `address`, `sync`, `address_service_internal` |
  | operations | `detail` |
  | app       | `detail`, `https_detail` |

- `history_batch` exists twice on purpose: the codec writers
  (`fortis::codec::history_batch`) and the price-history views built on them
  (`fortis::prices::history_batch`). Each module owns its half.
- File-local helpers stay in an anonymous namespace inside the module's
  namespace.

## Referring to another module

- Inside `fortis::<module>`, name another module relative to `fortis`:
  `http::HttpRequest`, `storage::KvStore`, `base::Clock`,
  `wallet::address::Store`. Code that already spelled `fortis::...` keeps the
  full form (`fortis::base::UniquePtr`).
- A header forward-declares another module's type in that module's namespace,
  never in its own: `namespace fortis::storage { class KvStore; }`.
- Short aliases are allowed only as `namespace x = fortis::y;` in a `.cpp`
  file, never in a header. The common ones: `tj` (`fortis::codec::tj`), `pw`
  (`fortis::codec::proto_wire`), `jj` (`fortis::compat::js_json`), `hb`
  (`fortis::codec::history_batch`), `cc` (`fortis::exchange::cross_chain`),
  `rh` (`fortis::exchange::robinhood`).

## What stays outside

- `main()` (`src/app/main.cpp`).
- `extern "C"` declarations and definitions (lwIP, Mbed TLS hooks, the guest
  C shims in `src/operations/guest_*_stats.*`, the linked landing image); their
  symbol names must not change. C structs they use (for example
  `FortisGuestSocketStats`) stay global and are forward-declared globally.
- C sources (`.c`) and `.inc` fragments are not checked.
- What `check-layers` rejects outside `fortis::<module>`: any other declaration
  (including `using` declarations and anonymous namespaces at file scope), a
  block in another module's namespace that holds more than forward
  declarations or names no existing module, and an `extern` block whose
  linkage is not `"C"` or that contains a namespace, class or template. A
  namespace alias in a header is rejected even inside `fortis::<module>`.

## Tests

Test code is not in a module. Test sources name production code with full
qualification or with `using namespace fortis::<module>;` after their
includes; test fakes of the base seams live in `fortis::testing`
(`tests/fake_seams.h`). The composition hooks of the replay binaries
(`tests/server/`) extend `fortis::app`.
