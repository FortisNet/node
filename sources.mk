# The one list of the server's sources, read by both builds: the host Makefile
# (which derives build/<name>.o from each entry) and the Unikraft guest
# build (which compiles each entry under $(APPINTELLIGENCECPP_BASE); its build
# files are not in this repository).
# Paths are relative to this directory, and the order is the link order of
# both builds. Add a new source file here and nowhere else;
# scripts/check_production_binary.py (make test) fails when a .c/.cpp under
# src/ is missing from this list or the list names a file that does not
# exist, and scripts/check_layers.py when FORTIS_SOURCE_DIRS does not name
# exactly the directories under src/.
#
# Only variable assignments belong here: the file is included by two
# different build systems. Compiler flags stay with each build system.

# Every directory under src/. Both builds put each on the include path.
FORTIS_SOURCE_DIRS := ai app auth base chains codec compat config crypto discovery exchange http networks news operations prices providers security storage tokens wallet

# Compiled for the guest only: C shims over Unikraft APIs with no host
# counterpart. The host Makefile leaves them out; they stay in FORTIS_SOURCES.
FORTIS_GUEST_ONLY_SOURCES := src/operations/guest_allocator_stats.c src/operations/guest_socket_stats.c

FORTIS_SOURCES :=
# The production entry point and the composition root (src/app), then the
# core server: configuration, HTTP, storage, device auth and the price
# providers. Nothing under tests/ may be listed: the test entry point, probes
# and hooks (tests/server) belong to the host replay binaries only.
FORTIS_SOURCES += src/app/main.cpp
FORTIS_SOURCES += src/app/server.cpp
FORTIS_SOURCES += src/app/application.cpp
FORTIS_SOURCES += src/app/http_listener.cpp
FORTIS_SOURCES += src/app/https_listener.cpp
FORTIS_SOURCES += src/app/shutdown_signal.cpp
FORTIS_SOURCES += src/app/probe_routes.cpp
FORTIS_SOURCES += src/app/module_configs.cpp
FORTIS_SOURCES += src/app/service_wiring.cpp
FORTIS_SOURCES += src/config/native_config.cpp
FORTIS_SOURCES += src/http/http_request.cpp
FORTIS_SOURCES += src/http/http_server.cpp
FORTIS_SOURCES += src/codec/protobuf_probe.cpp
FORTIS_SOURCES += src/storage/storage_probe.cpp
FORTIS_SOURCES += src/http/tls_probe.cpp
FORTIS_SOURCES += src/http/outbound_https.cpp
FORTIS_SOURCES += src/codec/bounded_json.cpp
FORTIS_SOURCES += src/providers/coingecko_price.cpp
FORTIS_SOURCES += src/providers/coingecko_contract_price.cpp
FORTIS_SOURCES += src/providers/coingecko_request.cpp
FORTIS_SOURCES += src/providers/jupiter_price.cpp
FORTIS_SOURCES += src/providers/jupiter_request.cpp
FORTIS_SOURCES += src/providers/vestige_price.cpp
FORTIS_SOURCES += src/providers/vestige_request.cpp
FORTIS_SOURCES += src/providers/kraken_price.cpp
FORTIS_SOURCES += src/providers/defillama_price.cpp
FORTIS_SOURCES += src/providers/mexc_price.cpp
FORTIS_SOURCES += src/providers/tronscan_price.cpp
FORTIS_SOURCES += src/providers/tronscan_request.cpp
FORTIS_SOURCES += src/providers/robinhood_price.cpp
FORTIS_SOURCES += src/providers/price_id_parser.cpp
FORTIS_SOURCES += src/prices/price_spot_cache.cpp
FORTIS_SOURCES += src/prices/price_series_store.cpp
FORTIS_SOURCES += src/prices/price_history_coverage.cpp
FORTIS_SOURCES += src/prices/price_history_backfill.cpp
FORTIS_SOURCES += src/prices/price_history_backfill_owner.cpp
FORTIS_SOURCES += src/prices/spot_response_serializer.cpp
FORTIS_SOURCES += src/prices/spot_resolution.cpp
FORTIS_SOURCES += src/prices/spot_provider_owner.cpp
FORTIS_SOURCES += src/prices/spot_owner_requests.cpp
FORTIS_SOURCES += src/prices/spot_owner_batches.cpp
FORTIS_SOURCES += src/prices/spot_owner_jupiter.cpp
FORTIS_SOURCES += src/prices/spot_owner_robinhood.cpp
FORTIS_SOURCES += src/providers/provider_lane.cpp
FORTIS_SOURCES += src/providers/provider_execution.cpp
FORTIS_SOURCES += src/providers/provider_execution_coordinator.cpp
FORTIS_SOURCES += src/storage/kv_store.cpp
FORTIS_SOURCES += src/storage/kv_index.cpp
FORTIS_SOURCES += src/storage/kv_log.cpp
FORTIS_SOURCES += src/storage/kv_recovery.cpp
FORTIS_SOURCES += src/storage/kv_commit.cpp
FORTIS_SOURCES += src/storage/kv_compaction.cpp
FORTIS_SOURCES += src/storage/kv_batch.cpp
FORTIS_SOURCES += src/storage/kv_runtime.cpp
FORTIS_SOURCES += src/storage/kv_probe.cpp
FORTIS_SOURCES += src/auth/device_auth.cpp
FORTIS_SOURCES += src/auth/device_challenge.cpp
FORTIS_SOURCES += src/auth/device_enrollment.cpp
FORTIS_SOURCES += src/auth/apple_app_attest.cpp
FORTIS_SOURCES += src/auth/apple_app_attest_root.cpp
FORTIS_SOURCES += src/auth/evm_proof.cpp
# nanopb and the checked-in generated messages (proto/generated).
FORTIS_SOURCES += third_party/nanopb/pb_common.c
FORTIS_SOURCES += third_party/nanopb/pb_decode.c
FORTIS_SOURCES += third_party/nanopb/pb_encode.c
FORTIS_SOURCES += proto/generated/fortisnet/intelligence/v1/dashpay.pb.c
FORTIS_SOURCES += proto/generated/fortisnet/intelligence/v1/devices.pb.c
# Play Integrity: the verdict validator, the Google
# OAuth/decode client and the required-mode Android enrollment issuer. The
# release sources carry no test transport; the host Makefile's test-seam
# variants of them are linked only into host test binaries.
FORTIS_SOURCES += src/auth/play_integrity.cpp
FORTIS_SOURCES += src/auth/play_integrity_google.cpp
FORTIS_SOURCES += src/auth/play_integrity_enrollment.cpp
FORTIS_SOURCES += src/operations/network_route.cpp
FORTIS_SOURCES += src/networks/network_registry.cpp
FORTIS_SOURCES += proto/generated/fortisnet/intelligence/v1/news.pb.c
FORTIS_SOURCES += proto/generated/fortisnet/intelligence/v1/networks.pb.c
# GET /v1/prices: one route object for every binary. Normal startup registers
# it only behind FORTIS_NATIVE_PRICE_ROUTE_CANARY=1.
FORTIS_SOURCES += src/prices/price_spot_route.cpp
FORTIS_SOURCES += src/prices/price_spot_query.cpp
FORTIS_SOURCES += src/prices/price_spot_service.cpp
FORTIS_SOURCES += src/prices/price_history_query.cpp
FORTIS_SOURCES += src/prices/price_ohlc_query.cpp
# GET /v1/prices/snapshot uses the existing spot service composition.
FORTIS_SOURCES += src/prices/price_snapshot.cpp
# GET/POST /v1/prices/history and GET /v1/prices/ohlc: one object per route
# for every binary, registered behind the same canary. Test binaries replace
# only the history owner objects (with their instrumentation hooks) and
# supply route seams.
FORTIS_SOURCES += src/prices/price_history_route.cpp
FORTIS_SOURCES += src/prices/price_history_batch_route.cpp
FORTIS_SOURCES += src/prices/price_history_service.cpp
FORTIS_SOURCES += src/prices/history_query_service.cpp
FORTIS_SOURCES += src/prices/price_series_repository.cpp
FORTIS_SOURCES += src/codec/price_history_batch_codec.cpp
FORTIS_SOURCES += src/codec/proto_wire.cpp
FORTIS_SOURCES += src/prices/price_history_view.cpp
FORTIS_SOURCES += src/discovery/ohlc_cache_policy.cpp
FORTIS_SOURCES += src/prices/ohlc_provider_owner.cpp
# /metrics allocator gauges: host builds collect nothing, but the formatter
# is shared so its bounds are tested natively.
FORTIS_SOURCES += src/operations/guest_allocator_metrics.cpp
# Guest-only (FORTIS_GUEST_ONLY_SOURCES): the allocator shim reads <uk/alloc.h>
# statistics and needs CONFIG_LIBUKALLOC_IFSTATS in the guest configuration.
FORTIS_SOURCES += src/operations/guest_allocator_stats.c
FORTIS_SOURCES += src/operations/guest_socket_stats.c
FORTIS_SOURCES += src/operations/guest_cpu_stats.c
# GET /metrics bearer policy, split from main.cpp so its edges are unit-tested.
FORTIS_SOURCES += src/operations/metrics_auth.cpp
# Operations: GET /, /assets/background.jpg, /statusz, /errors,
# /errorsz and POST /v1/crash-reports, the API telemetry ring they read, and
# the KV expiry sweeper. operations_pages.cpp owns the HTML templates directly;
# redaction_tables.cpp still follows the migration oracle. The landing
# background is embedded from assets/background.jpg.
FORTIS_SOURCES += src/operations/error_board.cpp
FORTIS_SOURCES += src/operations/error_board_route.cpp
FORTIS_SOURCES += src/operations/api_telemetry.cpp
FORTIS_SOURCES += src/operations/resource_snapshot.cpp
FORTIS_SOURCES += src/operations/operations_pages.cpp
FORTIS_SOURCES += src/operations/operations_routes.cpp
FORTIS_SOURCES += src/operations/release_routes.cpp
FORTIS_SOURCES += src/operations/sandbox_control.cpp
FORTIS_SOURCES += src/operations/sandbox_socket.cpp
FORTIS_SOURCES += src/operations/token_prices_route.cpp
FORTIS_SOURCES += src/operations/network_preview_route.cpp
FORTIS_SOURCES += src/operations/cache_explorer_route.cpp
FORTIS_SOURCES += src/operations/api_docs_generated.cpp
FORTIS_SOURCES += src/operations/landing_background.cpp
FORTIS_SOURCES += src/codec/crash_report_codec.cpp
FORTIS_SOURCES += src/operations/crash_report_route.cpp
FORTIS_SOURCES += src/security/diagnostic_redaction.cpp
FORTIS_SOURCES += src/security/redaction_tables.cpp
FORTIS_SOURCES += src/storage/kv_expiry_sweeper.cpp
# Discovery: POST /v1/bittensor/analytics, POST /v1/prices/markets
# and POST /v1/tokens/compare (registered behind the price canary), their
# shared provider owner, SWR cache, JSON/protobuf helpers and the generated
# Unicode case mapping data.
FORTIS_SOURCES += src/compat/js_case.cpp
FORTIS_SOURCES += src/compat/js_case_tables.cpp
FORTIS_SOURCES += src/codec/json_document_writer.cpp
FORTIS_SOURCES += src/discovery/discovery_cache.cpp
FORTIS_SOURCES += src/discovery/discovery_provider_owner.cpp
FORTIS_SOURCES += src/discovery/discovery_route_support.cpp
FORTIS_SOURCES += src/discovery/discovery_service.cpp
FORTIS_SOURCES += src/discovery/bittensor_route.cpp
FORTIS_SOURCES += src/discovery/market_view.cpp
FORTIS_SOURCES += src/compat/whatwg_url.cpp
FORTIS_SOURCES += src/compat/idna.cpp
FORTIS_SOURCES += src/compat/idna_tables.cpp
FORTIS_SOURCES += src/discovery/media_registry.cpp
FORTIS_SOURCES += src/discovery/price_markets_route.cpp
FORTIS_SOURCES += src/discovery/token_search_route.cpp
FORTIS_SOURCES += src/networks/token_ref.cpp
FORTIS_SOURCES += src/codec/text_sanitize.cpp
FORTIS_SOURCES += src/tokens/token_comparison.cpp
FORTIS_SOURCES += src/tokens/token_comparison_store.cpp
FORTIS_SOURCES += src/tokens/token_comparison_route.cpp
# News and media: GET /v1/news, /v1/news/:id, /v1/news/:id/reactions
# and /v1/media/:key with the news ingest and media prune jobs, composed by
# NewsService behind FORTIS_NATIVE_NEWS_CANARY=1.
FORTIS_SOURCES += src/compat/js_date_parse.cpp
FORTIS_SOURCES += src/compat/js_regex.cpp
FORTIS_SOURCES += src/news/news_text.cpp
FORTIS_SOURCES += src/news/news_json.cpp
FORTIS_SOURCES += src/news/news_store.cpp
FORTIS_SOURCES += src/news/news_routes.cpp
FORTIS_SOURCES += src/news/media_service.cpp
FORTIS_SOURCES += src/news/news_ingest.cpp
FORTIS_SOURCES += src/news/news_service.cpp
# The news read route units beside news_routes.cpp: they share
# news_routes_internal.h and have no header of their own.
FORTIS_SOURCES += src/news/news_reactions.cpp
FORTIS_SOURCES += src/news/news_reaction_prices.cpp
# Token, holder, liquidity and gems routes: the shared
# token JSON/text/domain ports, the durable token store, the token provider
# owner, the SWR cache, the Token360/page/history read models, the recurring
# jobs and TokenService, composed behind FORTIS_NATIVE_PRICE_ROUTE_CANARY=1.
FORTIS_SOURCES += src/codec/token_json.cpp
FORTIS_SOURCES += src/codec/provider_text.cpp
FORTIS_SOURCES += src/tokens/token_views.cpp
FORTIS_SOURCES += src/tokens/token_liquidity.cpp
FORTIS_SOURCES += src/tokens/token_evm.cpp
FORTIS_SOURCES += src/tokens/token_contract.cpp
FORTIS_SOURCES += src/tokens/token_data.cpp
FORTIS_SOURCES += src/tokens/token_writes.cpp
FORTIS_SOURCES += src/providers/token_provider.cpp
FORTIS_SOURCES += src/tokens/token_swr.cpp
FORTIS_SOURCES += src/tokens/token_fetchers.cpp
FORTIS_SOURCES += src/tokens/token_facts.cpp
FORTIS_SOURCES += src/tokens/token_fact_dependencies.cpp
FORTIS_SOURCES += src/tokens/token_facet_fetcher.cpp
FORTIS_SOURCES += src/tokens/token_facet_planner.cpp
FORTIS_SOURCES += src/tokens/token_facts_subfacets.cpp
FORTIS_SOURCES += src/tokens/token_facts_resolution.cpp
FORTIS_SOURCES += src/tokens/token_identity_resolver.cpp
FORTIS_SOURCES += src/tokens/token_facts_revalidation.cpp
FORTIS_SOURCES += src/tokens/token_fact_writer.cpp
FORTIS_SOURCES += src/tokens/token_tron.cpp
FORTIS_SOURCES += src/tokens/token_ledger.cpp
FORTIS_SOURCES += src/tokens/token_saucerswap.cpp
FORTIS_SOURCES += src/tokens/token_bittensor.cpp
FORTIS_SOURCES += src/tokens/token_robinhood.cpp
FORTIS_SOURCES += src/tokens/token_history.cpp
FORTIS_SOURCES += src/tokens/token_page.cpp
FORTIS_SOURCES += src/tokens/token_jobs.cpp
FORTIS_SOURCES += src/tokens/price_jobs.cpp
FORTIS_SOURCES += src/discovery/coin_popularity.cpp
FORTIS_SOURCES += src/prices/coin_price_scheduler.cpp
FORTIS_SOURCES += src/tokens/token_service.cpp
FORTIS_SOURCES += src/tokens/token_workspaces.cpp
FORTIS_SOURCES += src/tokens/token_price_access.cpp
FORTIS_SOURCES += src/tokens/token_request_worker.cpp
FORTIS_SOURCES += src/tokens/token_revalidator.cpp
FORTIS_SOURCES += src/tokens/token_job_scheduler.cpp
FORTIS_SOURCES += src/tokens/token_route_support.cpp
FORTIS_SOURCES += src/tokens/token_task_routes.cpp
FORTIS_SOURCES += src/tokens/token_history_routes.cpp
# Address, transaction and network routes: the chain
# vocabulary and transport, one driver/source/view file per family, the
# durable address store, the facets, the jobs and AddressService, composed
# behind FORTIS_NATIVE_ADDRESS_CANARY=1.
FORTIS_SOURCES += src/chains/chain_core.cpp
FORTIS_SOURCES += src/chains/chain_registry.cpp
FORTIS_SOURCES += src/chains/chain_transport.cpp
FORTIS_SOURCES += src/chains/json_rpc_client.cpp
FORTIS_SOURCES += src/chains/chain_evm.cpp
FORTIS_SOURCES += src/chains/chain_evm_view.cpp
FORTIS_SOURCES += src/chains/chain_utxo.cpp
FORTIS_SOURCES += src/chains/chain_utxo_view.cpp
FORTIS_SOURCES += src/chains/chain_cardano.cpp
FORTIS_SOURCES += src/chains/chain_cardano_view.cpp
FORTIS_SOURCES += src/chains/chain_solana.cpp
FORTIS_SOURCES += src/chains/chain_solana_view.cpp
FORTIS_SOURCES += src/chains/chain_tron.cpp
FORTIS_SOURCES += src/chains/chain_tron_token.cpp
FORTIS_SOURCES += src/chains/chain_asset_index.cpp
FORTIS_SOURCES += src/chains/chain_xrpl.cpp
FORTIS_SOURCES += src/chains/chain_stellar.cpp
FORTIS_SOURCES += src/chains/chain_algorand.cpp
FORTIS_SOURCES += src/chains/chain_hedera.cpp
FORTIS_SOURCES += src/chains/chain_sui.cpp
FORTIS_SOURCES += src/chains/chain_cosmos.cpp
FORTIS_SOURCES += src/chains/chain_aptos.cpp
FORTIS_SOURCES += src/chains/chain_icp.cpp
FORTIS_SOURCES += src/chains/chain_near.cpp
FORTIS_SOURCES += src/chains/chain_vechain.cpp
FORTIS_SOURCES += src/chains/chain_bittensor.cpp
FORTIS_SOURCES += src/wallet/address_records.cpp
FORTIS_SOURCES += src/wallet/address_store.cpp
FORTIS_SOURCES += src/wallet/address_facets.cpp
FORTIS_SOURCES += src/wallet/wallet_price_access.cpp
FORTIS_SOURCES += src/wallet/address_transfers.cpp
FORTIS_SOURCES += src/wallet/address_overview.cpp
FORTIS_SOURCES += src/wallet/address_jobs.cpp
FORTIS_SOURCES += src/wallet/address_service.cpp
FORTIS_SOURCES += src/wallet/address_route_support.cpp
FORTIS_SOURCES += src/wallet/address_account_routes.cpp
FORTIS_SOURCES += src/wallet/address_transfers_route.cpp
FORTIS_SOURCES += src/wallet/address_network_routes.cpp
FORTIS_SOURCES += src/wallet/sync_request.cpp
FORTIS_SOURCES += src/wallet/sync_journal.cpp
FORTIS_SOURCES += src/wallet/sync_scope.cpp
FORTIS_SOURCES += src/wallet/sync_route.cpp
# AI conversations: the signed-proof boundary, the
# snapshot schema and model context, the OpenAI exchange, the durable store
# and AiConversationService, composed behind FORTIS_NATIVE_AI_CANARY=1.
FORTIS_SOURCES += src/ai/ai_proof.cpp
FORTIS_SOURCES += src/ai/ai_snapshot.cpp
FORTIS_SOURCES += src/ai/ai_reply.cpp
FORTIS_SOURCES += src/ai/ai_stats.cpp
FORTIS_SOURCES += src/ai/ai_store.cpp
FORTIS_SOURCES += src/ai/ai_service.cpp
# Cross-chain exchange: the shared JavaScript-value and
# route helpers, the six provider adapters over one chain transport lease and
# CrossChainService, composed behind FORTIS_NATIVE_CROSS_CHAIN_CANARY=1.
FORTIS_SOURCES += src/exchange/cross_chain_support.cpp
FORTIS_SOURCES += src/exchange/cross_chain_providers.cpp
FORTIS_SOURCES += src/exchange/cross_chain_aggregators.cpp
FORTIS_SOURCES += src/exchange/cross_chain_maya.cpp
FORTIS_SOURCES += src/exchange/cross_chain_near_intents.cpp
FORTIS_SOURCES += src/exchange/cross_chain_simpleswap.cpp
FORTIS_SOURCES += src/exchange/cross_chain_bridge_vet.cpp
FORTIS_SOURCES += src/exchange/cross_chain_service.cpp
# Robinhood stock tokens and sponsored execution: the trade
# policy and diagnostics, the device tickets, the issuer read model and
# RobinhoodService, composed behind FORTIS_NATIVE_ROBINHOOD_CANARY=1.
FORTIS_SOURCES += src/exchange/robinhood_policy.cpp
FORTIS_SOURCES += src/exchange/robinhood_tickets.cpp
FORTIS_SOURCES += src/exchange/robinhood_markets.cpp
FORTIS_SOURCES += src/exchange/robinhood_service.cpp
FORTIS_SOURCES += src/exchange/robinhood_routes.cpp
FORTIS_SOURCES += src/exchange/robinhood_write_requests.cpp
# The proof-verified DashPay name resolver — the
# DPNS proof verifier over the vendored blst (BLS12-381) and BLAKE3, the
# trusted-quorum client and DashPayService, composed behind
# FORTIS_NATIVE_DASHPAY_CANARY=1. blst and BLAKE3 are upstream sources compiled
# unchanged (third_party/*/UPSTREAM.md): blst's C and pre-generated x86_64
# assembly in upstream's "portable" configuration without the CPU probe (the
# QEMU guest CPU has no SSSE3), BLAKE3's portable C behind the project's
# portable-only dispatcher (upstream's needs <immintrin.h>).
FORTIS_SOURCES += src/exchange/dashpay_proof.cpp
FORTIS_SOURCES += src/exchange/dashpay_client.cpp
FORTIS_SOURCES += src/exchange/dashpay_service.cpp
FORTIS_SOURCES += third_party/blst/v0.3.12/src/server.c
FORTIS_SOURCES += third_party/blst/v0.3.12/build/assembly.S
FORTIS_SOURCES += src/crypto/blst_platform_cap.c
FORTIS_SOURCES += third_party/blake3/1.8.7/blake3.c
FORTIS_SOURCES += src/crypto/blake3_portable_dispatch.c
FORTIS_SOURCES += third_party/blake3/1.8.7/blake3_portable.c

# Component source partitions in link order (consumers before dependencies).
# Both host archives and guest libraries consume these exact assignments.
FORTIS_COMPONENTS := app operations wallet ai tokens exchange news prices discovery chains networks providers auth http storage config codec compat security crypto base
FORTIS_COMPONENT_SOURCES_app := $(filter src/app/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_operations := $(filter src/operations/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_wallet := $(filter src/wallet/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_ai := $(filter src/ai/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_tokens := $(filter src/tokens/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_exchange := $(filter src/exchange/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_news := $(filter src/news/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_prices := $(filter src/prices/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_discovery := $(filter src/discovery/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_chains := $(filter src/chains/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_networks := $(filter src/networks/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_providers := $(filter src/providers/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_auth := $(filter src/auth/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_http := $(filter src/http/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_storage := $(filter src/storage/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_compat := $(filter src/compat/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_security := $(filter src/security/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_config := $(filter src/config/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_codec := $(filter src/codec/% third_party/nanopb/% proto/generated/%,$(FORTIS_SOURCES))
# Bundled backends share their owner's partial-link unit. In particular blst's
# assembly COMMON capability word must resolve with our aligned native definition
# before Unikraft allocates COMMON symbols to each component's BSS.
FORTIS_COMPONENT_SOURCES_crypto := $(filter src/crypto/% third_party/blst/% third_party/blake3/%,$(FORTIS_SOURCES))
FORTIS_COMPONENT_SOURCES_base := $(filter src/base/%,$(FORTIS_SOURCES))
