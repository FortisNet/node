CXX ?= c++
CC ?= cc
.DEFAULT_GOAL := all
THIS_MAKEFILE := $(lastword $(MAKEFILE_LIST))
# Every object and binary has a rule in this file; make's built-in suffix
# rules would only add implicit-rule searches for each header named by a .d.
.SUFFIXES:
# Exceptions are on (the compiler default) and RTTI is off, as in the guest
# build (-fexceptions -fno-rtti), so a dynamic_cast or typeid that the
# guest would reject fails here first. A CXXFLAGS override must keep -fno-rtti.
CXXFLAGS ?= -O2 -std=c++23 -fno-rtti -Wall -Wextra -Wpedantic -Werror -Wformat=2 -Wundef -Wcast-align -Woverloaded-virtual -Wnon-virtual-dtor -ffp-contract=off
CFLAGS ?= -O2 -std=c11 -Wall -Wextra -Werror -pedantic
HOST_PTHREAD_FLAGS = -pthread
CPPFLAGS += -DPB_FIELD_32BIT -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD -pthread -Ithird_party/nanopb -Iproto/generated -I.deps/mbedtls-3.6.7/include
# Mbed TLS user config (src/crypto, on the -Isrc/... path below): every object
# and the vendored library build agree on it; it disables MBEDTLS_SELF_TEST.
MBEDTLS_USER_CONFIG = src/crypto/fortis_mbedtls_config.h
CPPFLAGS += -DMBEDTLS_USER_CONFIG_FILE=\"$(notdir $(MBEDTLS_USER_CONFIG))\"

# sources.mk is the one list of the server's sources and of the src/
# directories. The guest build reads the same file, so the host binary and the
# guest image cannot drift: a new source file is added there and nowhere else.
include sources.mk
# Source headers keep their existing short include names. All basenames are
# unique, while their directories now reflect the component that owns them.
SOURCE_DIRS := $(FORTIS_SOURCE_DIRS)
CPPFLAGS += $(addprefix -Isrc/,$(SOURCE_DIRS))
# Header dependencies come from the compiler alone: build/<object>.d for an
# object and build/<binary>.bin.d for a test binary whose link recipe compiles
# its tests/ source. No rule in this file names a header; a rule lists more
# than its source only for what the compiler cannot see (a per-object flag, an
# embedded asset, a generated input).
DEPFLAGS = -MMD -MP -MF $(@:.o=.d) -MT $@
CPPFLAGS += $(if $(filter build/%.o,$@),$(DEPFLAGS),$(if $(filter build/%,$@),-MMD -MP -MF $@.bin.d -MT $@))
# A binary's .bin.d makes the headers of its tests/ source prerequisites too,
# so a link recipe names its inputs with this instead of $^.
LINK_INPUTS = $(filter %.cpp %.c %.o %.a,$^)
VPATH := $(addprefix src/,$(SOURCE_DIRS))
SOURCE_CPP_STEMS := $(notdir $(basename $(foreach dir,$(SOURCE_DIRS),$(wildcard src/$(dir)/*.cpp src/$(dir)/*.c))))
SOURCE_HEADER_NAMES := $(notdir $(foreach dir,$(SOURCE_DIRS),$(wildcard src/$(dir)/*.h)))
ifneq ($(words $(SOURCE_CPP_STEMS)),$(words $(sort $(SOURCE_CPP_STEMS))))
$(error Duplicate source basenames in src/; build objects require unique names)
endif
ifneq ($(words $(SOURCE_HEADER_NAMES)),$(words $(sort $(SOURCE_HEADER_NAMES))))
$(error Duplicate header basenames in src/; short includes require unique names)
endif

# The production objects: one per source in sources.mk, less the guest-only C
# shims. An object is named after its source; blst's server.c and assembly.S
# take a blst_ prefix (build/server.o belongs to src/app/server.cpp).
HOST_SOURCES = $(filter-out $(FORTIS_GUEST_ONLY_SOURCES),$(FORTIS_SOURCES))
OBJECTS := $(foreach source,$(HOST_SOURCES),build/$(if $(findstring /blst/,$(source)),blst_)$(basename $(notdir $(source))).o)

# Production component archives; test variants keep linking their explicit
# object selections so failure-injection flags and probe substitutions stay local.
component_host_sources = $(filter-out $(FORTIS_GUEST_ONLY_SOURCES),$(FORTIS_COMPONENT_SOURCES_$(1)))
component_objects = $(foreach source,$(call component_host_sources,$(1)),build/$(if $(findstring /blst/,$(source)),blst_)$(basename $(notdir $(source))).o)
COMPONENT_LIBRARIES := $(foreach component,$(FORTIS_COMPONENTS),$(if $(call component_host_sources,$(component)),build/libfortis_$(component).a))
# Retain the old all-object link semantics: registration/data-only members
# (notably blst's aligned capability word) must not disappear through archive
# extraction. Test variants already select objects explicitly as before.
FORTIS_HOST_SYSTEM := $(shell uname -s)
comma := ,
ifeq ($(FORTIS_HOST_SYSTEM),Darwin)
COMPONENT_LINK_INPUTS = $(foreach library,$(COMPONENT_LIBRARIES),-Wl$(comma)-force_load$(comma)$(library))
else
COMPONENT_LINK_INPUTS = -Wl,--whole-archive $(COMPONENT_LIBRARIES) -Wl,--no-whole-archive
endif
define fortis_host_component
build/libfortis_$(1).a: $(call component_objects,$(1)) sources.mk $(THIS_MAKEFILE) | build
	rm -f $$@.tmp
	$$(AR) rcs $$@.tmp $(call component_objects,$(1))
	mv $$@.tmp $$@
endef
$(foreach component,$(FORTIS_COMPONENTS),$(if $(call component_host_sources,$(component)),$(eval $(call fortis_host_component,$(component)))))
.PHONY: components print-component-sources
components: $(COMPONENT_LIBRARIES)
print-component-sources:
	@$(foreach component,$(FORTIS_COMPONENTS),printf '%s\n' '$(component):$(FORTIS_COMPONENT_SOURCES_$(component))';)

# The vendored Mbed TLS library. One recipe (the stamp's) builds the three
# archives, so a parallel make cannot run it twice. The stamp's name carries a
# checksum of the compiler and flags: the library is rebuilt when those, the
# user config or the pinned version (scripts/fetch-mbedtls.sh) change, and not
# when this Makefile is edited. A dry run (make -n) of an up-to-date tree
# prints nothing for it.
# Shared checkouts may build Linux guest host tools and macOS test binaries.
# Identical compiler names/flags must never reuse another platform's objects.
NATIVE_BUILD_PLATFORM := $(shell uname -s)-$(shell uname -m)
MBEDTLS_LIB = .deps/mbedtls-3.6.7/library
# The library recipe passes these through two shells (this recipe's double
# quotes, then the vendored Makefile's compile line), hence the triple escape.
MBEDTLS_CFLAGS = -O2 $(FORTIS_DEPENDENCY_CFLAGS) -DMBEDTLS_THREADING_C -DMBEDTLS_THREADING_PTHREAD $(HOST_PTHREAD_FLAGS) \
	-I../../../$(dir $(MBEDTLS_USER_CONFIG)) -DMBEDTLS_USER_CONFIG_FILE=\\\"$(notdir $(MBEDTLS_USER_CONFIG))\\\"
MBEDTLS_LIBS = $(addprefix $(MBEDTLS_LIB)/,libmbedtls.a libmbedx509.a libmbedcrypto.a)
MBEDTLS_STAMP := $(MBEDTLS_LIB)/.fortis-built-$(firstword $(shell printf '%s' '$(NATIVE_BUILD_PLATFORM) $(CC) $(MBEDTLS_CFLAGS)' | cksum))
# Order-only prerequisites of every compile: the Mbed TLS headers must be
# unpacked first. Their content is tracked through the .d files like any
# other header (the user config included).
COMPILE_ORDER = $(MBEDTLS_LIB)/Makefile

# The composition root (src/app). server.cpp (ServerMain) and the shared app
# units compile once; main.cpp is the production entry point, which passes
# ServerMain no composition hooks. build/fortis_node and the guest image
# link nothing from tests/: scripts/check_production_binary.py (make
# check-production-binary, part of make test) fails the build if a test probe,
# test hook or test-only name reaches the linked production binary.
#
# A replay server variant replaces build/main.o with the test entry point and
# probes under tests/server, compiled with the variant's FORTIS_*_TEST_ONLY
# flags: $(call app_variant_objects,<variant>) names them
# (build/<unit>_<variant>.o; rules beside each variant), and
# APP_TEST_SHARED_OBJECTS are the flag-independent test units every variant
# links (the /probe/async and /probe/provider-test probes, their provider
# bridge and the test-hook environment).
# The KV store is several units that share src/storage/kv_store_internal.h.
# Every binary links the whole set: the production objects, or the
# failure-injection objects (-DFORTIS_KV_TEST_FAILURES). The failure hooks are
# inline in the internal header, so the units of one set must agree.
KV_STORE_UNITS = kv_store kv_index kv_log kv_recovery kv_commit kv_compaction kv_batch
KV_STORE_OBJECTS = $(KV_STORE_UNITS:%=build/%.o)
KV_STORE_FAILURE_OBJECTS = $(KV_STORE_UNITS:%=build/%_failure_test.o)
# The spot provider owner is several units that share
# src/prices/spot_provider_owner_internal.h; every binary links all of them.
SPOT_PROVIDER_OWNER_OBJECTS = build/defillama_price.o build/mexc_price.o build/kraken_price.o build/network_registry.o build/spot_provider_owner.o build/spot_owner_requests.o build/spot_owner_batches.o build/spot_owner_jupiter.o build/spot_owner_robinhood.o
APP_TEST_UNITS = test_main test_probes_market test_probes_token test_probes_news test_probes_address test_probes_services
APP_TEST_OBJECTS = build/main.o
APP_TEST_SHARED_OBJECTS = build/test_probe_routes.o build/test_hook_env.o build/provider_http_bridge.o
app_variant_objects = $(addprefix build/,$(addsuffix _$(1).o,$(APP_TEST_UNITS)))

# Play Integrity: the verdict validator, the Google
# OAuth/decode client and the required-mode Android enrollment issuer. The
# release objects carry no test transport; the _test_seam variants below are
# linked only into host test binaries.
PLAY_INTEGRITY_OBJECTS = build/play_integrity.o build/play_integrity_google.o build/play_integrity_enrollment.o
# The landing page background, embedded by landing_background.cpp (.incbin).
LANDING_BACKGROUND = assets/background.jpg
# The tracker's own objects for binaries that link the spot owner alone.
COIN_POPULARITY_OBJECTS = build/coin_popularity.o build/js_case.o build/js_case_tables.o
# The proof-verified DashPay name resolver — the
# DPNS proof verifier over the vendored blst (BLS12-381) and BLAKE3, the
# trusted-quorum client and DashPayService, composed behind
# FORTIS_NATIVE_DASHPAY_CANARY=1. The vendored C is upstream code compiled
# unchanged (third_party/*/UPSTREAM.md), without the project's -Werror.
BLST_DIR = third_party/blst/v0.3.12
BLAKE3_DIR = third_party/blake3/1.8.7
# blst's upstream "portable" build without the CPU probe: the zero
# __blst_platform_cap keeps every optional instruction set (SSSE3 and SHA-NI
# SHA-256, ADX/BMI2 multiplication) off, so the code runs on baseline x86-64
# (the QEMU guest CPU has no SSSE3). Host and guest use the same flags.
BLST_FLAGS = -D__BLST_PORTABLE__ -D__BLST_NO_CPUID__
BLAKE3_FLAGS = -DBLAKE3_NO_SSE2 -DBLAKE3_NO_SSE41 -DBLAKE3_NO_AVX2 -DBLAKE3_NO_AVX512 -DBLAKE3_USE_NEON=0
DASHPAY_CRYPTO_OBJECTS = build/blst_server.o build/blst_assembly.o build/blst_platform_cap.o build/blake3.o build/blake3_portable_dispatch.o build/blake3_portable.o

# The object variables above name subsets of OBJECTS for the unit-test link
# lines; none of them adds an object to the server.
NAMED_OBJECT_SUBSETS = $(KV_STORE_OBJECTS) $(SPOT_PROVIDER_OWNER_OBJECTS) $(APP_TEST_OBJECTS) $(PLAY_INTEGRITY_OBJECTS) $(COIN_POPULARITY_OBJECTS) $(DASHPAY_CRYPTO_OBJECTS)
ifneq ($(filter-out $(OBJECTS),$(NAMED_OBJECT_SUBSETS)),)
$(error Not built from sources.mk: $(filter-out $(OBJECTS),$(NAMED_OBJECT_SUBSETS)))
endif

# The compile-flags stamp. Its name carries a checksum of everything that
# decides how an object is compiled: the compilers, the flag variables, and
# the text of this file's compile recipes (the per-object flags; for a recipe
# that compiles and links a test binary, its options). Every object, and every
# binary built from no object, has the stamp as a prerequisite, so changing a
# flag here, in the environment or on the command line rebuilds everything,
# and so does changing it back. Any other edit of this file (a comment, a new
# rule that reuses existing flags) leaves the checksum alone and rebuilds
# nothing. A new variable used by a compile recipe belongs in
# COMPILE_FLAG_VALUES.
COMPILE_FLAG_VALUES = $(NATIVE_BUILD_PLATFORM) | $(CXX) | $(CC) | $(CXXFLAGS) | $(CFLAGS) | $(CPPFLAGS) | $(HOST_PTHREAD_FLAGS) | $(BLST_DIR) $(BLST_FLAGS) | $(BLAKE3_DIR) $(BLAKE3_FLAGS) | $(abspath $(LANDING_BACKGROUND))
COMPILE_RECIPES_ID := $(firstword $(shell awk '/^\t\$$\((CC|CXX)\) / { if (/ -c /) print; else for (i = 2; i <= NF; i++) if ($$i ~ /^.?-[A-Za-z]/ && $$i != "-o") print $$i }' $(THIS_MAKEFILE) | sort -u | cksum))
FLAGS_STAMP := build/.flags-$(firstword $(shell printf '%s' '$(subst ','\'',$(COMPILE_FLAG_VALUES)) | $(COMPILE_RECIPES_ID)' | cksum))
# This make compares timestamps in whole seconds: the pause puts the new stamp
# in a later second than anything built just before the flags changed.
$(FLAGS_STAMP):
	@mkdir -p build
	@rm -f build/.flags-*
	@sleep 1
	@touch $@

build:
	@mkdir -p $@

.PHONY: generate-node-home check-node-home all test clean check-production-binary generate-network-catalog check-network-catalog generate-api-docs check-api-docs generate-redaction-tables check-redaction-tables generate-js-case-tables check-js-case-tables check-layers

all: check-network-catalog check-api-docs build/fortis_node

test: check-api-docs check-layers check-production-binary check-network-catalog check-redaction-tables check-js-case-tables

generate-network-catalog:
	node scripts/generate_network_catalog.mjs --write

check-network-catalog:
	node scripts/generate_network_catalog.mjs --check

generate-api-docs:
	node scripts/generate_api_docs.mjs --write

generate-node-home:
	python3 scripts/generate_node_home.py --write

check-node-home:
	python3 tests/generate_node_home_test.py
	python3 scripts/generate_node_home.py

check-api-docs: check-node-home
	node tests/route_bindings_test.mjs
	node scripts/generate_api_docs.mjs --check

generate-redaction-tables:
	node scripts/generate_redaction_tables.mjs --write

check-redaction-tables:
	node scripts/generate_redaction_tables.mjs --check

generate-js-case-tables:
	node scripts/generate_js_case_tables.mjs --write

check-js-case-tables:
	node scripts/generate_js_case_tables.mjs --check

# Module layering: src/ includes must follow the layer order in the script.
check-layers:
	python3 scripts/check_layers.py
	python3 scripts/check_component_boundaries.py --self-test

build/fortis_node: $(COMPONENT_LIBRARIES) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(COMPONENT_LINK_INPUTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# The production composition plus the test entry point with no plane flag:
# only the flag-independent probes (/probe/async, /probe/async-timeout and
# /probe/provider-test behind their FORTIS_ENABLE_* switches). The tests of
# the response handoff and the provider bridge run against it.
build/test_main_probe_test.o: tests/server/test_main.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -c $< -o $@
TEST_PROBE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS),$(OBJECTS)) build/test_main_probe_test.o $(APP_TEST_SHARED_OBJECTS)
build/fortis_node_probe_test: $(TEST_PROBE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_PROBE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# Fails when a test probe route, test hook or test-only name is linked into
# the production binary (the list is in the script).
check-production-binary: build/fortis_node build/fortis_node_probe_test
	python3 scripts/check_production_binary.py build/fortis_node build/fortis_node_probe_test

$(MBEDTLS_LIB)/Makefile:
	./scripts/fetch-mbedtls.sh

# The generated sources are made serially first: the vendored Makefile writes
# several of them from one multi-target rule, which -j would run twice.
$(MBEDTLS_STAMP): $(MBEDTLS_USER_CONFIG) scripts/fetch-mbedtls.sh | $(MBEDTLS_LIB)/Makefile
	rm -f $(MBEDTLS_LIB)/.fortis-*
	$(MAKE) -C $(MBEDTLS_LIB) clean
	$(MAKE) -C $(MBEDTLS_LIB) generated_files
	$(MAKE) -C $(MBEDTLS_LIB) -j4 static CFLAGS="$(MBEDTLS_CFLAGS)"
	touch $@

# The stamp is written after the archives, so each is brought forward once;
# from then on it is newer than the stamp and this recipe does not run.
$(MBEDTLS_LIBS): $(MBEDTLS_STAMP)
	@test -s $@ || { rm -f $(MBEDTLS_STAMP); echo "$@ was not built; run make again to rebuild Mbed TLS" >&2; exit 1; }
	@touch $@
# An archive deleted behind the stamp's back: the stamp no longer describes
# the tree, so its recipe runs again in this make (nothing is deleted while
# the makefile is read, and a dry run only prints the rebuild).
ifneq ($(words $(wildcard $(MBEDTLS_LIBS))),3)
.PHONY: $(MBEDTLS_STAMP)
endif
# Link lines that name libmbedtls.a as their prerequisite pass all three.
$(MBEDTLS_LIB)/libmbedtls.a: | $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a

# Every production object not given its own rule below: one C++ source under
# src/ (found through VPATH), the default flags.
build/%.o: %.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -c $< -o $@

# ── Operations ──
build/api_docs_generated.o: src/operations/api_docs_generated.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -Wno-overlength-strings -c $< -o $@
build/landing_background.o: src/operations/landing_background.cpp $(LANDING_BACKGROUND) $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) '-DFORTIS_LANDING_BACKGROUND_PATH="$(abspath $(LANDING_BACKGROUND))"' -c $< -o $@
build/diagnostic_redaction_test: tests/diagnostic_redaction_test.cpp build/diagnostic_redaction.o build/redaction_tables.o build/bounded_json.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
build/error_board_test: tests/error_board_test.cpp build/error_board.o build/network_registry.o $(KV_STORE_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(LINK_INPUTS) -o $@

build/resource_snapshot_test: tests/resource_snapshot_test.cpp build/resource_snapshot.o build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(filter %.cpp %.o,$^) -o $@

# Opt-in measurement; timing is evidence, never a correctness threshold.
test-prereqs: build/mexc_price_test build/price_fallback_warmer_test build/defillama_price_test build/kraken_price_test build/api_telemetry_benchmark

build/api_telemetry_benchmark: tests/api_telemetry_benchmark.cpp build/api_telemetry.o build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(filter %.cpp %.o,$^) -o $@

build/api_telemetry_test: tests/api_telemetry_test.cpp build/api_telemetry.o build/price_history_batch_codec.o build/proto_wire.o build/bounded_json.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(LINK_INPUTS) -o $@
build/crash_report_codec_test: tests/crash_report_codec_test.cpp build/crash_report_codec.o build/price_history_batch_codec.o build/proto_wire.o build/bounded_json.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
OPERATIONS_PAGES_TEST_OBJECTS = $(filter-out build/main.o,$(OBJECTS))
build/price_fallback_warmer_test: tests/price_fallback_warmer_fixture.cpp $(OPERATIONS_PAGES_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/operations_pages_test: tests/operations_pages_test.cpp $(OPERATIONS_PAGES_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@
$(call app_variant_objects,operations_test): build/%_operations_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_OPERATIONS_TEST_ONLY -c $< -o $@
build/device_auth_operations_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_OPERATIONS_TEST_ONLY -c $< -o $@
TEST_OPERATIONS_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,operations_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_operations_test.o
build/fortis_node_operations_test: $(TEST_OPERATIONS_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_OPERATIONS_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# ── Discovery ──

# The AI route test binary: the normal objects plus the test main (test
# clock, the fixture's ulid counter, a loopback OpenAI upstream and the two
# premium fixture devices).
$(call app_variant_objects,ai_route_test): build/%_ai_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_AI_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_ai_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_AI_ROUTE_TEST_ONLY -c $< -o $@
TEST_AI_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,ai_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_ai_route_test.o
build/fortis_node_ai_route_test: $(TEST_AI_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_AI_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/ai_store_test: tests/ai_store_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/ai_store_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/ai_messages_test: tests/ai_messages_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/ai_messages_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R38: Store::Open over the fault-injecting KV build (FORTIS_KV_TEST_FAIL).
build/ai_post_message_test: tests/ai_post_message_test.cpp $(filter-out build/main.o $(KV_STORE_OBJECTS),$(OBJECTS)) $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/ai_post_message_test.cpp $(filter-out build/main.o $(KV_STORE_OBJECTS),$(OBJECTS)) $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/ai_open_test: tests/ai_open_test.cpp $(filter-out build/main.o $(KV_STORE_OBJECTS),$(OBJECTS)) $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/ai_open_test.cpp $(filter-out build/main.o $(KV_STORE_OBJECTS),$(OBJECTS)) $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/ai_conversation_test: tests/ai_conversation_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/ai_conversation_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The cross-chain route test binary: the normal objects plus the test main
# (test clock, the four premium fixture devices, a loopback upstream for
# every provider host through the redirected transport).
$(call app_variant_objects,cross_chain_route_test): build/%_cross_chain_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_cross_chain_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_CROSS_CHAIN_ROUTE_TEST_ONLY -c $< -o $@
TEST_CROSS_CHAIN_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,cross_chain_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_cross_chain_route_test.o
build/fortis_node_cross_chain_route_test: $(TEST_CROSS_CHAIN_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_CROSS_CHAIN_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The Robinhood route test binary: the normal objects plus the test main
# (test clock, deterministic tickets, the eight premium fixture devices, a
# loopback upstream for every provider host through the redirected transport).
$(call app_variant_objects,robinhood_route_test): build/%_robinhood_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_ROBINHOOD_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_robinhood_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_ROBINHOOD_ROUTE_TEST_ONLY -c $< -o $@
TEST_ROBINHOOD_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,robinhood_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_robinhood_route_test.o
build/fortis_node_robinhood_route_test: $(TEST_ROBINHOOD_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_ROBINHOOD_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/dashpay_proof.o build/dashpay_client.o build/dashpay_service.o: build/%.o: %.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc -I$(BLST_DIR)/bindings -I$(BLAKE3_DIR) $(CXXFLAGS) -c $< -o $@
$(BLST_DIR)/build/assembly.S: scripts/fetch-blst-assembly.sh
	./scripts/fetch-blst-assembly.sh
build/blst_server.o: $(BLST_DIR)/src/server.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(FORTIS_DEPENDENCY_CFLAGS) -O2 $(BLST_FLAGS) -c $< -o $@
build/blst_assembly.o: $(BLST_DIR)/build/assembly.S $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(FORTIS_DEPENDENCY_ASFLAGS) $(BLST_FLAGS) -c $< -o $@
build/blake3.o: $(BLAKE3_DIR)/blake3.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(FORTIS_DEPENDENCY_CFLAGS) -O2 $(BLAKE3_FLAGS) -c $< -o $@
# The portable-only dispatcher (src/crypto/blake3_portable_dispatch.c) replaces
# upstream blake3_dispatch.c, whose x86 CPU probe needs <immintrin.h>.
# blst_platform_cap.c defines the capability word blst's assembly only
# declares as a common symbol (the file says why).
build/blake3_portable_dispatch.o build/blst_platform_cap.o: build/%.o: src/crypto/%.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(FORTIS_DEPENDENCY_CFLAGS) -O2 $(BLAKE3_FLAGS) -I$(BLAKE3_DIR) -Wall -Wextra -Werror -c $< -o $@
build/blake3_portable.o: $(BLAKE3_DIR)/blake3_portable.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(FORTIS_DEPENDENCY_CFLAGS) -O2 $(BLAKE3_FLAGS) -c $< -o $@
# The DashPay route test binary: the normal objects plus the test main
# (freshness clock, deterministic node choice, the malformed-identity hook,
# four premium fixture devices, a loopback upstream for the trusted quorum
# service and every evonode through the redirected transport).
$(call app_variant_objects,dashpay_route_test): build/%_dashpay_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_DASHPAY_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_dashpay_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_DASHPAY_ROUTE_TEST_ONLY -c $< -o $@
TEST_DASHPAY_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,dashpay_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_dashpay_route_test.o
build/fortis_node_dashpay_route_test: $(TEST_DASHPAY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_DASHPAY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/cross_chain_providers_test: tests/cross_chain_providers_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/cross_chain_providers_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/cross_chain_post_test: tests/cross_chain_post_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/cross_chain_post_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/network_route_unit_test: tests/network_route_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/network_route_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# GET|HEAD /v1/networks without the route lock: the real network handler and
# router with the development fixture devices and the rotation snapshot seam.
build/device_auth_network_concurrency_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_NETWORK_ROUTE_TEST_ONLY -DFORTIS_DEVICE_ROTATION_TEST_ONLY -c $< -o $@
NETWORK_CONCURRENCY_TEST_OBJECTS = $(filter-out build/main.o build/device_auth.o,$(OBJECTS)) build/device_auth_network_concurrency_test.o
build/network_catalog_concurrency_test: tests/network_catalog_concurrency_test.cpp $(NETWORK_CONCURRENCY_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) -DFORTIS_NETWORK_ROUTE_TEST_ONLY -DFORTIS_DEVICE_ROTATION_TEST_ONLY tests/network_catalog_concurrency_test.cpp $(NETWORK_CONCURRENCY_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# GET|HEAD /v1/prices without the route lock: the real spot handler, router,
# handoff and owner (StartRedirected against a loopback black hole), the price
# fixture devices, the rotation snapshot seam and the route's test seams.
build/device_auth_spot_concurrency_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_DEVICE_ROTATION_TEST_ONLY -c $< -o $@
build/price_spot_route_concurrency_test.o: src/prices/price_spot_route.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_SPOT_ROUTE_CONCURRENCY_TEST_ONLY -c $< -o $@
SPOT_READ_CONCURRENCY_TEST_OBJECTS = $(filter-out build/main.o build/device_auth.o build/price_spot_route.o,$(OBJECTS)) build/device_auth_spot_concurrency_test.o build/price_spot_route_concurrency_test.o
build/spot_read_concurrency_test: tests/spot_read_concurrency_test.cpp $(SPOT_READ_CONCURRENCY_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_DEVICE_ROTATION_TEST_ONLY -DFORTIS_SPOT_ROUTE_CONCURRENCY_TEST_ONLY tests/spot_read_concurrency_test.cpp $(SPOT_READ_CONCURRENCY_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/media_get_test: tests/media_get_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/media_get_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/dashpay_resolve_test: tests/dashpay_resolve_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/dashpay_resolve_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/dashpay_proof_test: tests/dashpay_proof_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/dashpay_proof_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R10: the sponsored-trade capability documents and their configuration gate.
build/robinhood_capabilities_test: tests/robinhood_capabilities_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/robinhood_capabilities_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/robinhood_stock_catalog_test: tests/robinhood_stock_catalog_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/robinhood_stock_catalog_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/robinhood_instrument_test: tests/robinhood_instrument_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/robinhood_instrument_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R43–R45: the Robinhood transaction POST routes below the HTTP layer.
build/robinhood_writes_test: tests/robinhood_writes_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/robinhood_writes_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/robinhood_corpus_test: tests/robinhood_corpus_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/robinhood_corpus_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/cross_chain_corpus_test: tests/cross_chain_corpus_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/cross_chain_corpus_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The token route test binary: the normal objects plus the test main
# (loopback token upstream, test clock, manual jobs, the price test route for
# the spot quote, the price history owners for the page series on the same
# upstream), the fixture-seeding device store, and the history backfill and
# OHLC owners with their instrumentation hooks. The token and spot owners are
# the normal objects, redirected at run time.
$(call app_variant_objects,token_route_test): build/%_token_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_TOKEN_ROUTE_TEST_ONLY -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_HISTORY_ROUTE_TEST_ONLY -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
TEST_TOKEN_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o build/price_history_backfill_owner.o build/ohlc_provider_owner.o,$(OBJECTS)) $(call app_variant_objects,token_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_price_route_test.o build/price_history_backfill_owner_route_test.o build/ohlc_provider_owner_route_test.o
build/fortis_node_token_route_test: $(TEST_TOKEN_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_TOKEN_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The address route test binary: the normal objects plus the test main
# (address + price test routes) and a premium fixture device. The chain
# transport and spot owner are the normal objects, redirected at run time.
$(call app_variant_objects,address_route_test): build/%_address_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_ADDRESS_ROUTE_TEST_ONLY -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_HISTORY_ROUTE_TEST_ONLY -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
build/device_auth_address_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_ADDRESS_ROUTE_TEST_ONLY -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_HISTORY_ROUTE_TEST_ONLY -c $< -o $@
TEST_ADDRESS_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o build/price_history_backfill_owner.o build/ohlc_provider_owner.o,$(OBJECTS)) $(call app_variant_objects,address_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_address_route_test.o build/price_history_backfill_owner_route_test.o build/ohlc_provider_owner_route_test.o build/prices.pb.o
build/fortis_node_address_route_test: $(TEST_ADDRESS_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_ADDRESS_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/token_comparison_seed.o: tests/token_comparison_seed.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -c $< -o $@
$(call app_variant_objects,discovery_route_test): build/%_discovery_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc -Itests $(CXXFLAGS) -DFORTIS_DISCOVERY_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_discovery_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_DISCOVERY_ROUTE_TEST_ONLY -c $< -o $@
TEST_DISCOVERY_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,discovery_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_discovery_route_test.o build/token_comparison_seed.o
build/fortis_node_discovery_route_test: $(TEST_DISCOVERY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_DISCOVERY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The shared concurrency primitives are header-only: no object to link.
build/file_descriptor_test: tests/file_descriptor_test.cpp src/base/file_descriptor.h
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $< -o $@

build/base_sync_test: tests/base_sync_test.cpp $(FLAGS_STAMP)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/base_sync_test.cpp -o $@
# The ownership helpers are header-only too.
build/base_ownership_test: tests/base_ownership_test.cpp $(FLAGS_STAMP)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/base_ownership_test.cpp -o $@
# So are Status / Result<T> and the logger.
build/base_status_test: tests/base_status_test.cpp $(FLAGS_STAMP)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/base_status_test.cpp -o $@
build/base_log_test: tests/base_log_test.cpp $(FLAGS_STAMP)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/base_log_test.cpp -o $@
build/js_case_test: tests/js_case_test.cpp build/js_case.o build/js_case_tables.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
build/js_json_test: tests/js_json_test.cpp build/js_json.o build/bounded_json.o build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@

# The unified hand-written protobuf writer against reference encoders and the
# golden bytes captured from the per-module writers it replaced.
build/proto_wire_golden_test: tests/proto_wire_golden_test.cpp build/js_json.o build/token_json.o build/bounded_json.o build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) -Isrc -Itests $(CXXFLAGS) tests/proto_wire_golden_test.cpp build/js_json.o build/token_json.o build/bounded_json.o build/price_history_batch_codec.o build/proto_wire.o -o $@
build/market_view_test: tests/market_view_test.cpp build/market_view.o build/whatwg_url.o build/idna.o build/idna_tables.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/price_history_batch_codec.o build/proto_wire.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
# The news route test binary: the normal objects plus the test main
# (loopback publishers/CoinGecko/media, clock, ids, manual jobs) and the
# fixture-seeding device store of the discovery replays. The discovery
# provider owner is the normal object, redirected at run time.
$(call app_variant_objects,news_route_test): build/%_news_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_NEWS_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_news_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_NEWS_ROUTE_TEST_ONLY -c $< -o $@
TEST_NEWS_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,news_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_news_route_test.o
build/fortis_node_news_route_test: $(TEST_NEWS_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_NEWS_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/whatwg_url_test: tests/whatwg_url_test.cpp build/whatwg_url.o build/idna.o build/idna_tables.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
build/token_ref_test: tests/token_ref_test.cpp build/token_ref.o build/text_sanitize.o build/price_history_batch_codec.o build/proto_wire.o build/bounded_json.o build/network_registry.o build/price_id_parser.o build/js_case.o build/js_case_tables.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
# The chain helper corpus links every object but main (the chain catalog
# resolves the family tables).
build/chain_core_test: tests/chain_core_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/chain_core_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/sync_request_test: tests/sync_request_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/sync_request_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R52: the sync route oracle in process. device_auth is the address test
# build's (premium fixture devices, as the socket replay seeds them).
build/sync_edges_test: tests/sync_edges_test.cpp $(filter-out build/main.o build/device_auth.o,$(OBJECTS)) build/device_auth_address_route_test.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) -DFORTIS_ADDRESS_ROUTE_TEST_ONLY tests/sync_edges_test.cpp $(filter-out build/main.o build/device_auth.o,$(OBJECTS)) build/device_auth_address_route_test.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/address_balances_test: tests/address_balances_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/address_balances_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/address_insight_test: tests/address_insight_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/address_insight_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/address_nfts_test: tests/address_nfts_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/address_nfts_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/tron_token_holdings_test: tests/tron_token_holdings_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/tron_token_holdings_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/address_transfers_test: tests/address_transfers_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/address_transfers_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/transaction_test: tests/transaction_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/transaction_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/transaction_view_test: tests/transaction_view_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/transaction_view_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/news_reads_test: tests/news_reads_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/news_reads_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/network_overview_test: tests/network_overview_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/network_overview_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/token360_test: tests/token360_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token360_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_history_test: tests/token_history_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token_history_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_page_test: tests/token_page_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token_page_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_search_test: tests/token_search_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token_search_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R40/R47 discovery POST units (PlanBittensorAnalytics, PriceMarketsIds/Render).
build/bittensor_analytics_test: tests/bittensor_analytics_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/bittensor_analytics_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/price_markets_test: tests/price_markets_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/price_markets_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# R53: the TypeScript edges oracle through the real route in process
# (socketpair dispatch, KV image restarts), plus concurrency and durability.
build/token_comparison_edges_test: tests/token_comparison_edges_test.cpp $(filter-out build/main.o build/device_auth.o,$(OBJECTS)) build/device_auth_discovery_route_test.o build/token_comparison_seed.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc -Itests $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) -DFORTIS_DISCOVERY_ROUTE_TEST_ONLY tests/token_comparison_edges_test.cpp $(filter-out build/main.o build/device_auth.o,$(OBJECTS)) build/device_auth_discovery_route_test.o build/token_comparison_seed.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
build/token_comparison_test: tests/token_comparison_test.cpp build/token_comparison.o build/token_comparison_store.o $(KV_STORE_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o build/js_case.o build/js_case_tables.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
# Atomic token writes (token_writes.h) against the KV with its test-only
# failure and kill points; `--measure` prints frame and latency figures.
# The Bittensor working set and formatter (token_bittensor.h); the
# projections reach chain_core, so it links every object but main.
build/token_bittensor_unit_test: tests/token_bittensor_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token_bittensor_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
# The Robinhood stock-token seams (token_robinhood.h, StockClient's FetchFn
# and the catalog it shares with the spot owner).
build/token_robinhood_unit_test: tests/token_robinhood_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/token_robinhood_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_atomicity_test: tests/token_atomicity_test.cpp build/token_writes.o build/token_data.o build/token_json.o build/token_comparison.o build/token_comparison_store.o $(KV_STORE_FAILURE_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o build/js_case.o build/js_case_tables.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -pthread -o $@

# The flag-independent test units of the replay server binaries.
build/test_hook_env.o: tests/server/test_hook_env.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server $(CXXFLAGS) -c $< -o $@
build/test_probe_routes.o: tests/server/test_probe_routes.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server $(CXXFLAGS) -c $< -o $@

$(call app_variant_objects,price_route_test): build/%_price_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -c $< -o $@

build/device_auth_price_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -c $< -o $@

# R33: the GET /v1/prices/ohlc query contract against the frozen TypeScript fixture.
build/price_ohlc_query_test: tests/price_ohlc_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/price_ohlc_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# R32: the GET /v1/prices/history query contract against the frozen TypeScript fixture.
build/price_history_query_test: tests/price_history_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/price_history_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# R31: the GET /v1/prices query contract against the frozen TypeScript fixture.
build/price_spot_query_test: tests/price_spot_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/price_spot_query_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

TEST_PRICE_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,price_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_price_route_test.o

build/fortis_node_price_route_test: $(TEST_PRICE_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_PRICE_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

$(call app_variant_objects,network_route_test): build/%_network_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_NETWORK_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_network_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_NETWORK_ROUTE_TEST_ONLY -c $< -o $@
build/network_registry.o: | check-network-catalog
build/network_registry_test.o: src/networks/network_registry.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -c $< -o $@
build/network_registry_test.o: | check-network-catalog
build/network_registry_catalog_test: tests/network_registry_test.cpp build/network_registry_test.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
TEST_NETWORK_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o,$(OBJECTS)) $(call app_variant_objects,network_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_network_route_test.o
build/fortis_node_network_route_test: $(TEST_NETWORK_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_NETWORK_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

$(call app_variant_objects,history_route_test): build/%_history_route_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_ROUTE_TEST_ONLY -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
build/ohlc_provider_owner_route_test.o: src/prices/ohlc_provider_owner.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
# The OHLC route's per-key cache timing table (in OBJECTS with the route).
build/ohlc_cache_policy_test: tests/ohlc_cache_policy_test.cpp build/ohlc_cache_policy.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
OHLC_OWNER_TEST_OBJECTS = $(filter-out build/main.o build/ohlc_provider_owner.o,$(OBJECTS))
build/ohlc_provider_owner_test: tests/ohlc_provider_owner_test.cpp build/ohlc_provider_owner_route_test.o $(OHLC_OWNER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS $< build/ohlc_provider_owner_route_test.o $(OHLC_OWNER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@
$(call app_variant_objects,combined_price_routes_test): build/%_combined_price_routes_test.o: tests/server/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server -Isrc $(CXXFLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_HISTORY_ROUTE_TEST_ONLY -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
build/device_auth_combined_price_routes_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_PRICE_ROUTE_TEST_ONLY -DFORTIS_HISTORY_ROUTE_TEST_ONLY -c $< -o $@
build/device_auth_history_route_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_ROUTE_TEST_ONLY -c $< -o $@
build/price_history_backfill_owner_route_test.o: src/prices/price_history_backfill_owner.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@
# The batch POST route's pure codec (in OBJECTS with the route).
# The GET/POST history encoder and gap plan shared by both history routes.
build/price_history_batch_codec_test: tests/price_history_batch_codec_test.cpp build/price_history_batch_codec.o build/proto_wire.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@
# The history routes, their codec, view and cache policy come from OBJECTS:
# the same objects the normal image links. Only main, device auth (fixture
# seeding) and the two owners (instrumentation hooks) are test builds.
TEST_HISTORY_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o build/price_history_backfill_owner.o build/ohlc_provider_owner.o,$(OBJECTS)) $(call app_variant_objects,history_route_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_history_route_test.o build/price_history_backfill_owner_route_test.o build/ohlc_provider_owner_route_test.o build/prices.pb.o
build/fortis_node_history_route_test: $(TEST_HISTORY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_HISTORY_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

TEST_COMBINED_PRICE_ROUTE_OBJECTS = $(filter-out $(APP_TEST_OBJECTS) build/device_auth.o build/price_history_backfill_owner.o build/ohlc_provider_owner.o,$(OBJECTS)) $(call app_variant_objects,combined_price_routes_test) $(APP_TEST_SHARED_OBJECTS) build/device_auth_combined_price_routes_test.o build/price_history_backfill_owner_route_test.o build/ohlc_provider_owner_route_test.o build/prices.pb.o
build/fortis_node_combined_price_routes_test: $(TEST_COMBINED_PRICE_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_COMBINED_PRICE_ROUTE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/guest_cpu_stats.o: src/operations/guest_cpu_stats.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(DEPFLAGS) $(CFLAGS) -c $< -o $@

build/robinhood_price_test: tests/robinhood_price_test.cpp build/robinhood_price.o build/bounded_json.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@

build/tronscan_price_test: tests/tronscan_price_test.cpp build/tronscan_price.o build/tronscan_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/tronscan_price_test.cpp build/tronscan_price.o build/tronscan_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/vestige_price_test: tests/vestige_price_test.cpp build/vestige_price.o build/vestige_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/vestige_price_test.cpp build/vestige_price.o build/vestige_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/jupiter_price_test: tests/jupiter_price_test.cpp build/jupiter_price.o build/jupiter_request.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/jupiter_price_test.cpp build/jupiter_price.o build/jupiter_request.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/price_history_coverage_test: tests/price_history_coverage_test.cpp build/price_history_coverage.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@

build/price_history_backfill_owner_test.o: src/prices/price_history_backfill_owner.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -c $< -o $@

build/price_history_backfill_test: tests/price_history_backfill_test.cpp build/price_history_backfill.o build/bounded_json.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(LINK_INPUTS) -o $@

build/price_history_backfill_owner_test: tests/price_history_backfill_owner_fixture.cpp build/price_history_backfill_owner_test.o build/price_history_backfill.o build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o build/native_config.o build/bounded_json.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS -DFORTIS_KV_TEST_FAILURES $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/provider_execution_coordinator_test: tests/provider_execution_coordinator_fixture.cpp build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $< build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/provider_http_bridge.o: tests/server/provider_http_bridge.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) -Itests/server $(CXXFLAGS) -c $< -o $@

# The Apple App Attest verifier. The release
# object trusts only the embedded official root (apple_app_attest_root.cpp);
# the _test_root object, compiled with FORTIS_APPLE_ATTEST_TEST_ROOT, trusts
# the root file named by FORTIS_TEST_APPLE_ATTEST_ROOT and is linked only
# into the test binaries below (never into build/fortis_node or the
# guest, where the flag is a compile error).

build/apple_app_attest_test_root.o: src/auth/apple_app_attest.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_APPLE_ATTEST_TEST_ROOT -c $< -o $@

APPLE_ATTEST_TEST_OBJECTS = build/apple_app_attest_root.o build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_FAILURE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o
build/apple_app_attest_test: tests/apple_app_attest_test.cpp build/apple_app_attest_test_root.o $(APPLE_ATTEST_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_APPLE_ATTEST_TEST_ROOT tests/apple_app_attest_test.cpp build/apple_app_attest_test_root.o $(APPLE_ATTEST_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/apple_app_attest_release_test: tests/apple_app_attest_test.cpp build/apple_app_attest.o $(APPLE_ATTEST_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/apple_app_attest_test.cpp build/apple_app_attest.o $(APPLE_ATTEST_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/apple_attest_fixture_tool: tests/apple_attest_fixture_tool.cpp $(FLAGS_STAMP) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/apple_attest_fixture_tool.cpp $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# The Play Integrity requestHash and decoded verdict validator (pure;
# linked into the server).
build/play_integrity_test: tests/play_integrity_test.cpp build/play_integrity.o build/bounded_json.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/play_integrity_test.cpp build/play_integrity.o build/bounded_json.o $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# The Google OAuth and decodeIntegrityToken client, in OBJECTS and the
# guest build. The _test_seam object adds
# FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT (injected transport, loopback endpoint
# and clocks) for host tests only; it is never in OBJECTS or the guest build.

build/play_integrity_google_test_seam.o: src/auth/play_integrity_google.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_PLAY_INTEGRITY_TEST_TRANSPORT -c $< -o $@

PLAY_INTEGRITY_GOOGLE_DEPS = build/play_integrity.o build/bounded_json.o build/outbound_https.o
build/play_integrity_google_test: tests/play_integrity_google_test.cpp build/play_integrity_google_test_seam.o $(PLAY_INTEGRITY_GOOGLE_DEPS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_PLAY_INTEGRITY_TEST_TRANSPORT tests/play_integrity_google_test.cpp build/play_integrity_google_test_seam.o $(PLAY_INTEGRITY_GOOGLE_DEPS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/play_integrity_google_release_test: tests/play_integrity_google_release_test.cpp build/play_integrity_google.o $(PLAY_INTEGRITY_GOOGLE_DEPS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/play_integrity_google_release_test.cpp build/play_integrity_google.o $(PLAY_INTEGRITY_GOOGLE_DEPS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# Required-mode Android enrollment. The release
# object is in OBJECTS; the _test_seam object (same source with
# FORTIS_PLAY_INTEGRITY_TEST_TRANSPORT) also reads
# FORTIS_TEST_PLAY_INTEGRITY_HOST/PORT and must be linked with
# play_integrity_google_test_seam.o, whose class layout it shares.

build/play_integrity_enrollment_test_seam.o: src/auth/play_integrity_enrollment.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_PLAY_INTEGRITY_TEST_TRANSPORT -c $< -o $@

# Unit groups: configuration loader, client initialization and the enrollment
# flow over an injected Google transport and the fault-injecting KV store.
PLAY_INTEGRITY_ENROLLMENT_TEST_DEPS = build/play_integrity_enrollment_test_seam.o build/play_integrity_google_test_seam.o $(PLAY_INTEGRITY_GOOGLE_DEPS) build/device_auth.o build/device_challenge.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_FAILURE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o
build/play_integrity_enrollment_test: tests/play_integrity_enrollment_test.cpp $(PLAY_INTEGRITY_ENROLLMENT_TEST_DEPS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_PLAY_INTEGRITY_TEST_TRANSPORT tests/play_integrity_enrollment_test.cpp $(PLAY_INTEGRITY_ENROLLMENT_TEST_DEPS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# The server whose Google client talks to a loopback TLS stub, for
# tests/play_integrity_enrollment_socket_test.py. Never a release artifact.
TEST_PLAY_INTEGRITY_OBJECTS = $(filter-out build/play_integrity_google.o build/play_integrity_enrollment.o,$(OBJECTS)) build/play_integrity_google_test_seam.o build/play_integrity_enrollment_test_seam.o
build/fortis_node_play_integrity_test: $(TEST_PLAY_INTEGRITY_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_PLAY_INTEGRITY_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# Focused Play Integrity suites: validator; client with generated keys,
# CA and a local TLS stub; enrollment units and the real-socket route over
# the same stub; release isolation of the server binary and guest list.
.PHONY: test-play-integrity
test-play-integrity: build/play_integrity_test build/play_integrity_google_test build/play_integrity_google_release_test build/play_integrity_google.o build/play_integrity_google_test_seam.o build/play_integrity_enrollment_test build/fortis_node_play_integrity_test build/fortis_node
	./build/play_integrity_test
	python3 tests/play_integrity_google_test.py build/play_integrity_google_test build/play_integrity_google_release_test build/play_integrity_google.o build/play_integrity_google_test_seam.o build/fortis_node
	python3 tests/play_integrity_enrollment_socket_test.py build/play_integrity_enrollment_test build/fortis_node_play_integrity_test build/fortis_node

# The server with the test-root verifier, for tests/apple_attest_socket_test.py.
TEST_APPLE_ATTEST_OBJECTS = $(filter-out build/apple_app_attest.o,$(OBJECTS)) build/apple_app_attest_test_root.o
build/fortis_node_apple_attest_test: $(TEST_APPLE_ATTEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_APPLE_ATTEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/outbound_https_test: tests/outbound_https_fixture.cpp build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/outbound_https_fixture.cpp build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/provider_lane_test: tests/provider_lane_test.cpp build/provider_lane.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/provider_lane_test.cpp build/provider_lane.o -o $@

build/provider_execution_test: tests/provider_execution_test.cpp build/provider_execution.o build/provider_lane.o build/outbound_https.o build/coingecko_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/provider_execution_test.cpp build/provider_execution.o build/provider_lane.o build/outbound_https.o build/coingecko_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/mexc_price_test: tests/mexc_price_test.cpp build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/mexc_price_test.cpp build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a $(HOST_PTHREAD_FLAGS) -o $@

build/defillama_price_test: tests/defillama_price_test.cpp build/defillama_price.o build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/defillama_price_test.cpp build/defillama_price.o build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a $(HOST_PTHREAD_FLAGS) -o $@

build/kraken_price_test: tests/kraken_price_test.cpp build/kraken_price.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kraken_price_test.cpp build/kraken_price.o build/bounded_json.o build/outbound_https.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a $(HOST_PTHREAD_FLAGS) -o $@

build/coingecko_price_test: tests/coingecko_price_test.cpp build/coingecko_price.o build/bounded_json.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/coingecko_price_test.cpp build/coingecko_price.o build/bounded_json.o -o $@

build/coingecko_contract_price_test: tests/coingecko_contract_price_test.cpp build/coingecko_contract_price.o build/coingecko_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/coingecko_contract_price_test.cpp build/coingecko_contract_price.o build/coingecko_request.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/bounded_json.o build/outbound_https.o build/native_config.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/device_auth_test: tests/device_auth_test.cpp build/device_auth.o $(KV_STORE_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/device_auth_test.cpp build/device_auth.o $(KV_STORE_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/device_challenge_test: tests/device_challenge_test.cpp build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/device_challenge_test.cpp build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

DEVICE_ENROLLMENT_TEST_OBJECTS = build/device_enrollment.o build/apple_app_attest.o build/apple_app_attest_root.o $(PLAY_INTEGRITY_OBJECTS) build/outbound_https.o build/bounded_json.o build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_OBJECTS) build/pb_common.o build/pb_decode.o build/pb_encode.o build/devices.pb.o
build/device_enrollment_test: tests/device_enrollment_test.cpp $(DEVICE_ENROLLMENT_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/device_enrollment_test.cpp $(DEVICE_ENROLLMENT_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# KV compare-and-write, one-use challenge and
# attested records. Uses the fault-injecting store object.
build/device_attestation_foundation_test: tests/device_attestation_foundation_test.cpp build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_FAILURE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/device_attestation_foundation_test.cpp build/device_challenge.o build/device_auth.o build/http_server.o build/http_request.o build/kv_runtime.o $(KV_STORE_FAILURE_OBJECTS) build/pb_common.o build/pb_encode.o build/devices.pb.o $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# Atomic bearer rotation. device_auth with the FORTIS_DEVICE_ROTATION_TEST_ONLY
# seam (snapshot hook, forced token, FORTIS_TEST_ROTATION_PAUSE_MS and
# FORTIS_TEST_ROTATION_COMPETE) is linked
# only into these test binaries, never into build/fortis_node or the
# guest (the flag is a compile error under FORTIS_UNIKRAFT).
build/device_auth_rotation_test.o: src/auth/device_auth.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_DEVICE_ROTATION_TEST_ONLY -c $< -o $@

build/device_rotation_test: tests/device_rotation_test.cpp build/device_auth_rotation_test.o $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_DEVICE_ROTATION_TEST_ONLY tests/device_rotation_test.cpp build/device_auth_rotation_test.o $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

build/device_transaction_contract_test: tests/device_transaction_contract_test.cpp build/device_auth.o $(KV_STORE_FAILURE_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) -pthread -o $@

# The server with the rotation seam, for tests/device_rotation_race_test.py.
TEST_ROTATION_OBJECTS = $(filter-out build/device_auth.o,$(OBJECTS)) build/device_auth_rotation_test.o
build/fortis_node_rotation_test: $(TEST_ROTATION_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $(TEST_ROTATION_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/evm_proof_test: tests/evm_proof_test.cpp build/evm_proof.o $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/evm_proof_test.cpp build/evm_proof.o $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/kv_store_test: tests/kv_store_test.cpp $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_store_test.cpp $(KV_STORE_OBJECTS) -o $@

build/kv_concurrency_test: tests/kv_concurrency_test.cpp $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_concurrency_test.cpp $(KV_STORE_OBJECTS) -o $@

$(KV_STORE_FAILURE_OBJECTS): build/%_failure_test.o: src/storage/%.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_KV_TEST_FAILURES -c $< -o $@

build/kv_failure_test: tests/kv_failure_test.cpp $(KV_STORE_FAILURE_OBJECTS) build/kv_expiry_sweeper.o build/kv_runtime.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) -o $@

build/test_support_test: tests/test_support_test.cpp tests/test_support.h | build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/test_support_test.cpp -o $@

build/kv_capacity_benchmark: tests/kv_capacity_benchmark.cpp $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_KV_TEST_FAILURES $(LINK_INPUTS) -pthread -o $@

build/kv_compaction_test: tests/kv_compaction_test.cpp $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_compaction_test.cpp $(KV_STORE_FAILURE_OBJECTS) -pthread -o $@

build/kv_commit_slots_test: tests/kv_commit_slots_test.cpp $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_commit_slots_test.cpp $(KV_STORE_FAILURE_OBJECTS) -pthread -o $@

build/kv_checksum_test: tests/kv_checksum_test.cpp $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_checksum_test.cpp $(KV_STORE_OBJECTS) -pthread -o $@

build/kv_compaction_soak: tests/kv_compaction_soak.cpp $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_compaction_soak.cpp $(KV_STORE_OBJECTS) -pthread -o $@

build/kv_backup_fixture: tests/kv_backup_fixture.cpp $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_backup_fixture.cpp $(KV_STORE_OBJECTS) -o $@

build/kv_runtime_test: tests/kv_runtime_test.cpp build/kv_runtime.o $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_runtime_test.cpp build/kv_runtime.o $(KV_STORE_OBJECTS) -o $@

build/kv_runtime_concurrency_test: tests/kv_runtime_concurrency_test.cpp build/kv_runtime.o $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/kv_runtime_concurrency_test.cpp build/kv_runtime.o $(KV_STORE_OBJECTS) -o $@

build/price_spot_cache_test: tests/price_spot_cache_test.cpp build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_spot_cache_test.cpp build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS) -o $@

build/price_series_store_test: tests/price_series_store_test.cpp build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_series_store_test.cpp build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS) -o $@

build/price_provider_batch_test: tests/price_provider_batch_test.cpp build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_provider_batch_test.cpp build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o $(KV_STORE_FAILURE_OBJECTS) -o $@

build/price_series_store_benchmark: tests/price_series_store_benchmark.cpp build/price_series_store.o build/price_history_coverage.o $(KV_STORE_OBJECTS)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_series_store_benchmark.cpp build/price_series_store.o build/price_history_coverage.o $(KV_STORE_OBJECTS) -o $@

build/price_id_parser_test: tests/price_id_parser_test.cpp build/price_id_parser.o build/js_case.o build/js_case_tables.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_id_parser_test.cpp build/price_id_parser.o build/js_case.o build/js_case_tables.o -o $@

build/spot_response_serializer_test: tests/spot_response_serializer_test.cpp build/spot_response_serializer.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/spot_response_serializer_test.cpp build/spot_response_serializer.o -o $@

build/spot_resolution_test: tests/spot_resolution_test.cpp build/spot_resolution.o build/defillama_price.o build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_request.o build/native_config.o build/outbound_https.o build/bounded_json.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/spot_resolution_test.cpp build/spot_resolution.o build/defillama_price.o build/mexc_price.o build/price_id_parser.o build/js_case.o build/js_case_tables.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_request.o build/native_config.o build/outbound_https.o build/bounded_json.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/spot_provider_owner_test: tests/spot_provider_owner_fixture.cpp $(COIN_POPULARITY_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o $(SPOT_PROVIDER_OWNER_OBJECTS) build/spot_resolution.o build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o build/coingecko_request.o build/jupiter_price.o build/jupiter_request.o build/vestige_price.o build/vestige_request.o build/tronscan_price.o build/tronscan_request.o build/robinhood_price.o build/native_config.o build/price_id_parser.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_contract_price.o build/bounded_json.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/spot_provider_owner_fixture.cpp $(COIN_POPULARITY_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o $(SPOT_PROVIDER_OWNER_OBJECTS) build/spot_resolution.o build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o build/coingecko_request.o build/jupiter_price.o build/jupiter_request.o build/vestige_price.o build/vestige_request.o build/tronscan_price.o build/tronscan_request.o build/robinhood_price.o build/native_config.o build/price_id_parser.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_contract_price.o build/bounded_json.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# Price jobs: the TypeScript refresh/seal replay and the bounded
# seal checks (tests/price_jobs_test.py drives it and serves CoinGecko).
PRICE_JOBS_FIXTURE_OBJECTS = $(COIN_POPULARITY_OBJECTS) build/price_jobs.o build/token_data.o build/token_json.o build/price_history_batch_codec.o build/proto_wire.o $(SPOT_PROVIDER_OWNER_OBJECTS) build/spot_resolution.o build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane.o build/outbound_https.o build/coingecko_request.o build/jupiter_price.o build/jupiter_request.o build/vestige_price.o build/vestige_request.o build/tronscan_price.o build/tronscan_request.o build/robinhood_price.o build/native_config.o build/price_id_parser.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_contract_price.o build/bounded_json.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o
build/price_jobs_fixture: tests/price_jobs_fixture.cpp $(PRICE_JOBS_FIXTURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/price_jobs_fixture.cpp $(PRICE_JOBS_FIXTURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# Coin popularity (RecurringJob 8): the TypeScript replay driver
# and the 20,000-coin load measurement (tests/coin_popularity_test.py drives
# it and serves CoinGecko /coins/markets). Every object but main, with the
# loopback-target token provider owner.
COIN_POPULARITY_FIXTURE_OBJECTS = $(filter-out build/main.o $(KV_STORE_OBJECTS),$(OBJECTS)) $(KV_STORE_FAILURE_OBJECTS)
build/coin_popularity_fixture: tests/coin_popularity_fixture.cpp $(COIN_POPULARITY_FIXTURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/coin_popularity_fixture.cpp $(COIN_POPULARITY_FIXTURE_OBJECTS) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# Keep queued saturation cases alive long enough to observe coordinator Busy
# retries; request rate, burst, concurrency, and breaker settings stay default.
build/provider_lane_combined_test.o: src/providers/provider_lane.cpp $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -DFORTIS_PROVIDER_TEST_COINGECKO_QUEUE_TIMEOUT_MS=30000 -c $< -o $@

build/combined_provider_owners_test: tests/combined_provider_owners_fixture.cpp $(COIN_POPULARITY_OBJECTS) build/price_history_batch_codec.o build/proto_wire.o $(SPOT_PROVIDER_OWNER_OBJECTS) build/price_history_backfill_owner_test.o build/spot_resolution.o build/provider_execution_coordinator.o build/provider_execution.o build/provider_lane_combined_test.o build/outbound_https.o build/coingecko_request.o build/jupiter_price.o build/jupiter_request.o build/vestige_price.o build/vestige_request.o build/tronscan_price.o build/tronscan_request.o build/robinhood_price.o build/native_config.o build/price_id_parser.o build/price_spot_cache.o build/price_series_store.o build/price_history_coverage.o build/coingecko_price.o build/coingecko_contract_price.o build/bounded_json.o build/price_history_backfill.o $(KV_STORE_OBJECTS) build/spot_response_serializer.o $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) -DFORTIS_HISTORY_BACKFILL_TEST_HOOKS $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -pthread -o $@

# The probe-lock test runs the real KV and nanopb probe calls.
HTTP_SERVER_TEST_OBJECTS = build/http_server.o build/http_request.o build/kv_probe.o build/kv_runtime.o $(KV_STORE_OBJECTS) build/protobuf_probe.o build/pb_common.o build/pb_decode.o build/pb_encode.o build/dashpay.pb.o
build/http_boundary_fuzz_test: tests/http_boundary_fuzz_test.cpp build/http_request.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $< build/http_request.o -o $@

build/ingress_raii_failure_test: tests/ingress_raii_failure_test.cpp tests/https_test_support.h $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/https_tls_unit_test: tests/https_tls_unit_test.cpp tests/https_test_support.h tests/unit_test_support.h $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/https_listener_unit_test: tests/https_listener_unit_test.cpp tests/unit_test_support.h $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/incoming_https_fixture: tests/incoming_https_fixture.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/http_resource_test: tests/http_resource_test.cpp build/http_request.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $< build/http_request.o -o $@

build/http_server_test: tests/http_server_test.cpp $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/http_server_test.cpp $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

CACHE_CONTRACT_OBJECTS = build/discovery_cache.o build/token_swr.o build/ohlc_cache_policy.o build/device_auth.o $(KV_STORE_OBJECTS) build/token_json.o build/price_history_batch_codec.o build/proto_wire.o
build/sync_scope_unit_test: tests/sync_scope_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/sync_scope_unit_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/fixed_string_test: tests/fixed_string_test.cpp src/base/fixed_string.h $(FLAGS_STAMP) | build
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $< -o $@

build/repair_candidates_test: tests/repair_candidates_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/repair_candidates_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/chain_id_set_test: tests/chain_id_set_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/chain_id_set_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/price_factory_test: tests/price_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/history_factory_test: tests/history_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/history_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/discovery_factory_test: tests/discovery_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/discovery_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/news_factory_test: tests/news_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/news_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_factory_test: tests/token_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/token_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/address_factory_test: tests/address_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/address_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/ai_factory_test: tests/ai_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/ai_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/cross_chain_factory_test: tests/cross_chain_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/cross_chain_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/robinhood_factory_test: tests/robinhood_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/robinhood_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/dashpay_factory_test: tests/dashpay_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/dashpay_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/adapter_factory_test: tests/adapter_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/adapter_factory_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/factory_contract_test: tests/factory_contract_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/factory_contract_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/history_ownership_test: tests/history_ownership_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/history_ownership_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/infrastructure_components_test: tests/infrastructure_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/infrastructure_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/content_components_test: tests/content_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/content_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/price_exchange_components_test: tests/price_exchange_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/price_exchange_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/chain_wallet_components_test: tests/chain_wallet_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/chain_wallet_components_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_group1_test: tests/token_group1_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/token_group1_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_identity_resolver_test: tests/token_identity_resolver_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/token_identity_resolver_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/token_facet_planner_test: tests/token_facet_planner_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/token_facet_planner_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/boundary_value_test: tests/boundary_value_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/boundary_value_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/callable_binding_test: tests/callable_binding_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/callable_binding_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/exception_boundary_test: tests/exception_boundary_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/exception_boundary_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/http_transport_unit_test: tests/http_transport_unit_test.cpp tests/unit_test_support.h $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $< $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/http_reply_unit_test: tests/http_reply_unit_test.cpp $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/http_reply_unit_test.cpp $(HTTP_SERVER_TEST_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/cache_contract_test: tests/cache_contract_test.cpp $(CACHE_CONTRACT_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/cache_contract_test.cpp $(CACHE_CONTRACT_OBJECTS) $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/unicode_contract_test: tests/unicode_contract_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/unicode_contract_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/app_post_decode_test: tests/app_post_decode_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) tests/app_post_decode_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

build/proto_request_reader_test: tests/proto_request_reader_test.cpp build/proto_wire.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/proto_request_reader_test.cpp build/proto_wire.o -o $@

build/query_string_test: tests/query_string_test.cpp src/http/query_string.h $(FLAGS_STAMP)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/query_string_test.cpp -o $@

build/native_config_test: tests/native_config_test.cpp build/native_config.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) tests/native_config_test.cpp build/native_config.o -o $@

build/guest_allocator_metrics_test: tests/guest_allocator_metrics_test.cpp build/guest_allocator_metrics.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/guest_allocator_metrics_test.cpp build/guest_allocator_metrics.o -o $@

build/metrics_auth_test: tests/metrics_auth_test.cpp build/metrics_auth.o
	$(CXX) $(CPPFLAGS) -Isrc $(CXXFLAGS) tests/metrics_auth_test.cpp build/metrics_auth.o -o $@

build/pb_%.o: third_party/nanopb/pb_%.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

# The checked-in nanopb messages (proto/generated).
build/%.pb.o: proto/generated/fortisnet/intelligence/v1/%.pb.c $(FLAGS_STAMP) | $(COMPILE_ORDER)
	@mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) -c $< -o $@

test-prereqs: build/ingress_raii_failure_test build/file_descriptor_test build/http_transport_unit_test build/https_tls_unit_test build/https_listener_unit_test build/http_resource_test build/http_boundary_fuzz_test build/incoming_https_fixture

test-prereqs: build/resource_snapshot_test build/error_board_test build/test_support_test build/history_ownership_test build/infrastructure_components_test build/content_components_test build/price_exchange_components_test build/chain_wallet_components_test build/token_group1_test build/token_identity_resolver_test build/token_facet_planner_test build/fixed_string_test build/adapter_factory_test build/price_factory_test build/history_factory_test build/discovery_factory_test build/news_factory_test build/token_factory_test build/address_factory_test build/ai_factory_test build/cross_chain_factory_test build/robinhood_factory_test build/dashpay_factory_test build/factory_contract_test build/chain_id_set_test build/repair_candidates_test build/boundary_value_test build/callable_binding_test build/exception_boundary_test build/cache_contract_test build/sync_scope_unit_test build/http_reply_unit_test build/unicode_contract_test build/app_post_decode_test build/proto_request_reader_test build/query_string_test build/device_transaction_contract_test build/price_snapshot_test build/fortis_node build/fortis_node_probe_test build/fortis_node_price_route_test build/fortis_node_network_route_test build/fortis_node_history_route_test build/fortis_node_combined_price_routes_test build/network_registry_catalog_test build/network_route_unit_test build/network_catalog_concurrency_test build/spot_read_concurrency_test build/outbound_https_test build/provider_lane_test build/provider_execution_test build/provider_execution_coordinator_test build/ohlc_provider_owner_test build/ohlc_cache_policy_test build/price_history_batch_codec_test build/coingecko_price_test build/coingecko_contract_price_test build/jupiter_price_test build/vestige_price_test build/tronscan_price_test build/robinhood_price_test build/kv_store_test build/kv_concurrency_test build/kv_failure_test build/kv_compaction_test build/kv_commit_slots_test build/kv_checksum_test build/kv_compaction_soak build/kv_backup_fixture build/kv_runtime_test build/kv_runtime_concurrency_test build/price_spot_cache_test build/price_series_store_test build/price_history_coverage_test build/price_history_backfill_test build/price_history_backfill_owner_test build/combined_provider_owners_test build/price_provider_batch_test build/price_id_parser_test build/price_spot_query_test build/price_history_query_test build/price_ohlc_query_test build/spot_response_serializer_test build/spot_resolution_test build/spot_provider_owner_test build/evm_proof_test build/device_auth_test build/device_challenge_test build/device_enrollment_test build/device_attestation_foundation_test build/device_rotation_test build/fortis_node_rotation_test build/apple_app_attest_test build/apple_app_attest_release_test build/apple_attest_fixture_tool build/fortis_node_apple_attest_test build/play_integrity_enrollment_test build/fortis_node_play_integrity_test build/http_server_test build/native_config_test build/guest_allocator_metrics_test build/metrics_auth_test build/fortis_node_operations_test build/diagnostic_redaction_test build/api_telemetry_test build/crash_report_codec_test build/operations_pages_test build/fortis_node_discovery_route_test build/base_sync_test build/base_ownership_test build/base_status_test build/base_log_test build/js_case_test build/js_json_test build/proto_wire_golden_test build/market_view_test build/token_ref_test build/token_comparison_test build/token_comparison_edges_test build/token_atomicity_test build/token_bittensor_unit_test build/token_robinhood_unit_test build/whatwg_url_test build/fortis_node_news_route_test build/news_reads_test build/media_get_test build/fortis_node_token_route_test build/fortis_node_address_route_test build/chain_core_test build/sync_request_test build/sync_edges_test build/address_balances_test build/address_insight_test build/address_nfts_test build/tron_token_holdings_test build/address_transfers_test build/transaction_test build/transaction_view_test build/network_overview_test build/token360_test build/token_history_test build/token_page_test build/token_search_test build/bittensor_analytics_test build/price_markets_test build/fortis_node_ai_route_test build/ai_conversation_test build/ai_messages_test build/fortis_node_cross_chain_route_test build/cross_chain_corpus_test build/cross_chain_providers_test build/cross_chain_post_test build/fortis_node_robinhood_route_test build/robinhood_corpus_test build/robinhood_capabilities_test build/robinhood_stock_catalog_test build/robinhood_instrument_test build/robinhood_writes_test build/dashpay_proof_test build/dashpay_resolve_test build/fortis_node_dashpay_route_test build/price_jobs_fixture build/coin_popularity_fixture

.PHONY: test-prereqs
test: test-prereqs test-current-contracts
	./build/adapter_factory_test
	./build/price_factory_test
	./build/history_factory_test
	./build/discovery_factory_test
	./build/news_factory_test
	./build/token_factory_test
	./build/address_factory_test
	./build/ai_factory_test
	./build/cross_chain_factory_test
	./build/robinhood_factory_test
	./build/dashpay_factory_test
	./build/factory_contract_test
	./build/chain_id_set_test
	./build/history_ownership_test
	./build/infrastructure_components_test
	./build/content_components_test
	./build/price_exchange_components_test
	./build/chain_wallet_components_test
	./build/token_group1_test
	./build/token_identity_resolver_test
	./build/token_facet_planner_test
	./build/fixed_string_test
	./build/repair_candidates_test
	./build/boundary_value_test
	./build/callable_binding_test
	./build/exception_boundary_test
	./build/cache_contract_test
	./build/sync_scope_unit_test
	./build/http_reply_unit_test
	./build/unicode_contract_test
	./build/app_post_decode_test
	python3 tests/app_post_wire_test.py build/fortis_node --compare tests/fixtures/app_post_schema_baseline.json --strict-types
	./build/proto_request_reader_test
	./build/query_string_test
	node tests/network_catalog_generator_test.mjs
	./build/network_registry_catalog_test
	python3 tests/kv_backup_test.py
	python3 tests/smoke.py build/fortis_node build/fortis_node_probe_test
	python3 tests/device_challenge_race_test.py build/fortis_node
	python3 tests/device_rotation_race_test.py build/fortis_node build/fortis_node_rotation_test
	python3 tests/apple_attest_socket_test.py build/fortis_node_apple_attest_test build/fortis_node build/apple_attest_fixture_tool
	python3 tests/apple_attest_release_check.py build/fortis_node build/fortis_node_apple_attest_test
	python3 tests/play_integrity_enrollment_socket_test.py build/play_integrity_enrollment_test build/fortis_node_play_integrity_test build/fortis_node
	python3 tests/auth_before_body_test.py build/fortis_node
	python3 tests/metrics_route_test.py build/fortis_node
	python3 tests/price_spot_canary_test.py build/fortis_node build/fortis_node_price_route_test
	python3 tests/price_history_canary_test.py build/fortis_node build/fortis_node_history_route_test
	python3 tests/live_price_canary_classification_test.py
	python3 tests/price_spot_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_jupiter_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_asa_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_trc10_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_query_route_test.py build/fortis_node_price_route_test
	python3 tests/network_route_test.py build/fortis_node_network_route_test
	./build/network_route_unit_test
	./build/network_catalog_concurrency_test
	./build/spot_read_concurrency_test
	python3 tests/network_route_edges_test.py build/fortis_node_network_route_test build/fortis_node_address_route_test build/fortis_node_token_route_test
	python3 tests/price_history_route_test.py build/fortis_node_history_route_test
	python3 tests/price_history_query_route_test.py build/fortis_node_history_route_test
	python3 tests/price_ohlc_query_route_test.py build/fortis_node_history_route_test
	python3 tests/price_ohlc_cache_policy_test.py build/fortis_node_history_route_test
	./build/price_history_batch_codec_test
	python3 tests/price_history_batch_route_test.py build/fortis_node_history_route_test
	python3 tests/price_history_batch_edge_route_test.py build/fortis_node_history_route_test
	python3 tests/price_history_decade_route_test.py build/fortis_node_history_route_test
	python3 tests/combined_price_routes_test.py build/fortis_node_combined_price_routes_test
	python3 tests/tls_ca.py build/fortis_node
	python3 tests/ca_bundle_test.py build/fortis_node
	python3 tests/outbound_https_test.py build/outbound_https_test
	./build/provider_lane_test
	python3 tests/provider_execution_test.py build/provider_execution_test
	python3 tests/provider_execution_coordinator_test.py build/provider_execution_coordinator_test
	./build/ohlc_provider_owner_test
	./build/ohlc_cache_policy_test
	python3 tests/spot_provider_owner_test.py build/spot_provider_owner_test
	./build/mexc_price_test
	./build/defillama_price_test
	./build/kraken_price_test
	./build/coingecko_price_test
	./build/coingecko_contract_price_test
	./build/jupiter_price_test
	./build/vestige_price_test
	./build/tronscan_price_test
	./build/robinhood_price_test
	python3 tests/provider_http_bridge_test.py build/fortis_node_probe_test
	./build/kv_store_test
	./build/kv_concurrency_test
	./build/kv_failure_test
	./build/kv_compaction_test
	./build/kv_compaction_test --parallel-point-reads
	./build/kv_compaction_test --commit-slots
	./build/kv_compaction_test --commit-slots --parallel-point-reads
	./build/kv_commit_slots_test
	./build/kv_checksum_test
	./build/kv_compaction_soak 21 0.25
	./build/kv_runtime_test
	./build/kv_runtime_concurrency_test
	./build/price_snapshot_test
	python3 tests/price_core_warmer_test.py build/coin_popularity_fixture
	./build/price_spot_cache_test
	./build/price_series_store_test
	./build/price_history_coverage_test
	./build/price_history_backfill_test tests/corpus/coingecko_history_backfill.json tests/corpus/coingecko_history_365d_daily.json
	python3 tests/price_history_backfill_owner_test.py build/price_history_backfill_owner_test tests/corpus/coingecko_history_backfill.json tests/corpus/coingecko_history_365d_daily.json
	python3 tests/combined_provider_owners_test.py build/combined_provider_owners_test
	./build/price_provider_batch_test
	./build/price_id_parser_test
	./build/price_spot_query_test tests/corpus/price_spot_query_route.json
	./build/price_history_query_test tests/corpus/price_history_query_route.json
	./build/price_ohlc_query_test tests/corpus/price_ohlc_query_route.json
	./build/spot_response_serializer_test
	./build/spot_resolution_test
	./build/evm_proof_test
	./build/device_auth_test
	./build/device_challenge_test
	./build/device_enrollment_test
	./build/device_attestation_foundation_test
	./build/device_rotation_test
	./build/device_transaction_contract_test
	./build/device_transaction_contract_test --group-commit
	./build/device_transaction_contract_test --parallel-point-reads
	./build/device_transaction_contract_test --commit-slots --group-commit --parallel-point-reads
	./build/apple_app_attest_test
	./build/apple_app_attest_release_test
	./build/http_server_test
	./build/http_resource_test
	./build/http_boundary_fuzz_test
	./build/https_listener_unit_test
	./build/https_tls_unit_test
	./build/http_transport_unit_test
	./build/native_config_test
	./build/sandbox_ipc_test
	./build/guest_allocator_metrics_test
	./build/metrics_auth_test
	./build/diagnostic_redaction_test tests/corpus/crash_redaction_corpus.json
	./build/error_board_test
	node tests/incorrect_coin_ids_page_test.mjs
	./build/api_telemetry_test tests/corpus/operations_telemetry.json
	./build/crash_report_codec_test
	./build/resource_snapshot_test
	./build/operations_pages_test
	python3 tests/crash_reports_route_test.py build/fortis_node_operations_test
	python3 tests/kv_compaction_service_test.py build/fortis_node_operations_test build/fortis_node
	./build/ingress_raii_failure_test
	./build/file_descriptor_test
	./build/base_sync_test
	./build/base_ownership_test
	./build/test_support_test
	python3 tests/quote_contract_expectations_test.py
	python3 tests/lint_tool_test.py
	./build/base_status_test
	./build/base_log_test
	node tests/js_case_test.mjs build/js_case_test
	node tests/js_json_test.mjs build/js_json_test
	./build/proto_wire_golden_test
	node tests/market_view_test.mjs build/market_view_test
	./build/token_ref_test tests/corpus/token_ref_corpus.json
	./build/token_comparison_test
	./build/token_atomicity_test
	./build/token_bittensor_unit_test
	./build/token_robinhood_unit_test
	python3 tests/token_comparison_route_test.py build/fortis_node_discovery_route_test
	./build/token_comparison_edges_test tests/corpus/token_comparison_edges.json
	python3 tests/token_comparison_edges_test.py build/fortis_node_discovery_route_test
	python3 tests/token_routes_test.py build/fortis_node_token_route_test
	python3 tests/token_series_test.py build/fortis_node_token_route_test
	python3 tests/token_solana_test.py build/fortis_node_token_route_test
	python3 tests/token_tron_test.py build/fortis_node_token_route_test
	python3 tests/token_ledger_test.py build/fortis_node_token_route_test
	python3 tests/token_issuer_test.py build/fortis_node_token_route_test
	python3 tests/token_hedera_test.py build/fortis_node_token_route_test
	python3 tests/token_saucerswap_test.py build/fortis_node_token_route_test
	python3 tests/token_sui_test.py build/fortis_node_token_route_test
	python3 tests/token_bittensor_test.py build/fortis_node_token_route_test
	python3 tests/token_robinhood_test.py build/fortis_node_token_route_test
	python3 tests/token_routes_native_test.py build/fortis_node build/fortis_node_token_route_test
	python3 tests/price_jobs_test.py build/price_jobs_fixture
	python3 tests/price_jobs_service_test.py build/fortis_node_token_route_test
	python3 tests/coin_popularity_test.py build/coin_popularity_fixture
	python3 tests/coin_popularity_service_test.py build/fortis_node_token_route_test
	python3 tests/coin_popularity_markets_test.py build/fortis_node_discovery_route_test
	./build/chain_core_test tests/corpus/chain_core_corpus.json
	python3 tests/address_routes_test.py build/fortis_node_address_route_test
	python3 tests/address_routes_native_test.py build/fortis_node build/fortis_node_address_route_test
	./build/address_balances_test tests/corpus/address_balances_edges.json
	python3 tests/address_balances_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/address_insight_test tests/corpus/address_insight_edges.json
	python3 tests/address_insight_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/address_nfts_test tests/corpus/address_nfts_edges.json
	python3 tests/address_nfts_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/tron_token_holdings_test tests/corpus/tron_token_holdings_edges.json
	python3 tests/tron_token_holdings_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/address_transfers_test tests/corpus/address_transfers_edges.json
	python3 tests/address_transfers_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/transaction_test tests/corpus/transaction_edges.json
	python3 tests/transaction_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/transaction_view_test tests/corpus/transaction_view_edges.json
	python3 tests/transaction_view_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/network_overview_test tests/corpus/network_overview_edges.json
	python3 tests/network_overview_edges_test.py build/fortis_node_address_route_test build/fortis_node
	./build/token360_test tests/corpus/token360_edges.json
	python3 tests/token360_edges_test.py build/fortis_node_token_route_test build/fortis_node
	./build/token_history_test tests/corpus/token_history_edges.json --measure
	python3 tests/token_history_edges_test.py build/fortis_node_token_route_test
	./build/token_page_test tests/corpus/token_page_edges.json
	python3 tests/token_page_edges_test.py build/fortis_node_token_route_test build/fortis_node
	./build/sync_request_test tests/corpus/sync_datetime_corpus.json
	python3 tests/sync_route_test.py build/fortis_node_address_route_test
	python3 tests/sync_route_native_test.py build/fortis_node build/fortis_node_address_route_test
	./build/sync_edges_test tests/corpus/sync_edges.json
	python3 tests/sync_edges_test.py build/fortis_node_address_route_test
	./build/ai_conversation_test tests/corpus/ai_conversation_corpus.json
	./build/ai_messages_test tests/corpus/ai_conversations_route.json
	python3 tests/ai_conversations_route_test.py build/fortis_node_ai_route_test
	python3 tests/ai_open_route_test.py build/fortis_node_ai_route_test
	python3 tests/ai_post_message_route_test.py build/fortis_node_ai_route_test
	python3 tests/ai_messages_get_route_test.py build/fortis_node_ai_route_test
	python3 tests/ai_conversations_native_test.py build/fortis_node build/fortis_node_ai_route_test
	python3 tests/ai_durable_thread_route_test.py build/fortis_node_ai_route_test
	python3 tests/server_timing_native_test.py build/fortis_node_probe_test
	python3 tests/helpdesk_removed_test.py build/fortis_node
	./build/cross_chain_corpus_test tests/corpus/cross_chain_corpus.json
	python3 tests/cross_chain_route_test.py build/fortis_node_cross_chain_route_test
	python3 tests/cross_chain_native_test.py build/fortis_node build/fortis_node_cross_chain_route_test
	./build/cross_chain_providers_test tests/corpus/cross_chain_providers_edges.json
	python3 tests/cross_chain_providers_route_test.py build/fortis_node_cross_chain_route_test
	./build/cross_chain_post_test tests/corpus/cross_chain_quotes_edges.json tests/corpus/cross_chain_status_edges.json
	python3 tests/cross_chain_post_edges_test.py build/fortis_node_cross_chain_route_test build/fortis_node
	./build/robinhood_corpus_test tests/corpus/robinhood_corpus.json
	python3 tests/robinhood_route_test.py build/fortis_node_robinhood_route_test
	python3 tests/robinhood_native_test.py build/fortis_node build/fortis_node_robinhood_route_test
	./build/robinhood_capabilities_test
	python3 tests/robinhood_capabilities_edges_test.py build/fortis_node_robinhood_route_test build/fortis_node
	./build/robinhood_stock_catalog_test tests/corpus/robinhood_stock_catalog_edges.json
	python3 tests/robinhood_stock_catalog_edges_test.py build/fortis_node_robinhood_route_test build/fortis_node
	./build/robinhood_instrument_test tests/corpus/robinhood_instrument_edges.json
	python3 tests/robinhood_instrument_edges_test.py build/fortis_node_robinhood_route_test build/fortis_node
	./build/robinhood_writes_test tests/corpus/robinhood_prepare_edges.json tests/corpus/robinhood_submit_edges.json tests/corpus/robinhood_diagnose_edges.json
	python3 tests/robinhood_writes_edges_test.py build/fortis_node_robinhood_route_test build/fortis_node
	./build/dashpay_proof_test tests/corpus/dashpay_proof_corpus.json
	./build/dashpay_resolve_test tests/corpus/dashpay_resolve_edges.json tests/corpus/dashpay_route.json
	python3 tests/dashpay_route_test.py build/fortis_node_dashpay_route_test
	python3 tests/dashpay_resolve_route_test.py build/fortis_node_dashpay_route_test
	python3 tests/dashpay_native_test.py build/fortis_node build/fortis_node_dashpay_route_test
	python3 tests/bittensor_analytics_route_test.py build/fortis_node_discovery_route_test
	python3 tests/price_markets_route_test.py build/fortis_node_discovery_route_test
	./build/bittensor_analytics_test tests/corpus/bittensor_analytics_edges.json
	python3 tests/bittensor_analytics_edges_test.py build/fortis_node_discovery_route_test build/fortis_node
	./build/price_markets_test tests/corpus/price_markets_edges.json
	python3 tests/price_markets_case_mapping_test.py build/fortis_node_discovery_route_test
	python3 tests/price_markets_edges_test.py build/fortis_node_discovery_route_test build/fortis_node
	python3 tests/token_search_route_test.py build/fortis_node_discovery_route_test
	./build/token_search_test tests/corpus/token_search_edges.json
	python3 tests/token_search_edges_test.py build/fortis_node_discovery_route_test build/fortis_node
	node tests/whatwg_url_test.mjs build/whatwg_url_test
	python3 tests/news_media_native_test.py build/fortis_node_news_route_test build/fortis_node
	./build/news_reads_test tests/corpus/news_reads_edges.json
	python3 tests/news_reads_edges_test.py build/fortis_node_news_route_test build/fortis_node
	./build/media_get_test tests/corpus/media_get_edges.json
	python3 tests/media_get_route_test.py build/fortis_node_news_route_test

clean:
	rm -rf build

# ── Sanitizers and lint (docs/sanitizers.md) ──
# Separate targets, not part of `make test`: they are slow. Each sanitizer
# variant is built out of tree by scripts/sanitize.py, in build-<variant>/
# with its own objects and its own Mbed TLS, by running this Makefile there
# with the variant's CXX/CC/CXXFLAGS/CFLAGS. build/, .deps/ and the flags
# stamp of the normal build are not touched. On macOS the variants build and
# run in the Dockerfile.sanitizers container (Linux, clang 18).
#   asan   AddressSanitizer + UBSan (+ LeakSanitizer)     build-asan/
#   ubsan  UBSan alone                                    build-ubsan/
#   tsan   ThreadSanitizer                                build-tsan/
# sanitize-test and tsan-test run the unit-test binaries of the test recipe
# (its ./build/... commands; UNIT='regex' narrows them) and fail on any
# report. sanitize-replay runs the python replays that match REPLAY against
# the variant's server binaries: make sanitize-replay REPLAY='dashpay|support'
# [SANITIZER=asan|ubsan|tsan].
SANITIZER ?= asan
.PHONY: asan ubsan tsan sanitize-test ubsan-test tsan-test sanitize-replay sanitize-clean
asan ubsan tsan:
	python3 scripts/sanitize.py $@ build
sanitize-test:
	python3 scripts/sanitize.py asan unit $(if $(UNIT),'$(UNIT)')
ubsan-test:
	python3 scripts/sanitize.py ubsan unit $(if $(UNIT),'$(UNIT)')
tsan-test:
	python3 scripts/sanitize.py tsan unit $(if $(UNIT),'$(UNIT)')
sanitize-replay:
	@test -n '$(REPLAY)' || { echo "usage: make sanitize-replay REPLAY='regex of python3 recipe commands' [SANITIZER=asan]" >&2; exit 2; }
	python3 scripts/sanitize.py $(SANITIZER) replay '$(REPLAY)'
sanitize-clean:
	rm -rf build-asan build-ubsan build-tsan

# clang-tidy (.clang-tidy) over the files changed against LINT_BASE (default:
# the merge-base with master; LINT_BASE=HEAD for uncommitted work). `lint`
# fails only on findings on the lines changed against that base ("zero new
# warnings"); LINT_WHOLE_FILES=1 reports every finding in the changed files.
# lint-all covers all of src/ and tests/ and fails on any finding. format-diffstat
# measures what .clang-format would change; the tree is not reformatted
# with it yet.
.PHONY: lint lint-all format-diffstat print-compile-flags print-mbedtls-stamp
# The complete pre-push gate; isolated real Clang/GCC builds, two analyzers,
# full clang-tidy lint and native unit tests. See docs/verification.md.
.PHONY: test-current-contracts
test-current-contracts: test-prereqs
	python3 tests/price_fallback_warmer_test.py build/price_fallback_warmer_test
	python3 tests/incoming_https_test.py build/fortis_node --handoff-binary build/incoming_https_fixture
	python3 tests/error_board_route_test.py build/fortis_node_operations_test
	python3 tests/operations_routes_test.py build/fortis_node_operations_test
	python3 tests/release_routes_test.py build/fortis_node_operations_test
	python3 tests/statusz_route_test.py build/fortis_node_operations_test
	python3 tests/ai_anthropic_async_route_test.py build/fortis_node_ai_route_test
	python3 tests/price_spot_contract_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_robinhood_route_test.py build/fortis_node_price_route_test
	python3 tests/price_spot_mixed_integration_route_test.py build/fortis_node_price_route_test
	python3 tests/price_ohlc_back_pressure_test.py build/fortis_node_history_route_test

.PHONY: verify lean-verify cppcheck verify-tool-tests
verify:
	python3 scripts/verify.py
# Clang build plus lint only; writes build-verify/report-lean.json.
lean-verify:
	python3 scripts/verify.py --stage lean
cppcheck:
	python3 scripts/verify.py --stage cppcheck
verify-tool-tests:
	python3 tests/native_release_test.py
	python3 tests/native_hardening_test.py
	python3 tests/core_contracts_compile_test.py
	python3 tests/cpp_contracts_tool_test.py
	python3 tests/stack_budget_tool_test.py
	shellcheck scripts/build-native-release.sh scripts/sign-native-release.sh
	python3 tests/portfolio_load_tool_test.py
	python3 tests/quote_contract_expectations_test.py
	python3 tests/verify_tool_test.py
	node tests/redaction_generator_test.mjs
lint:
	python3 scripts/lint.py tidy $(if $(LINT_BASE),--base '$(LINT_BASE)') $(if $(LINT_WHOLE_FILES),--whole-files)
lint-all:
	python3 tests/lint_tool_test.py
	python3 scripts/lint.py tidy --all
format-diffstat:
	python3 scripts/lint.py format-diffstat
# For scripts/lint.py: the default compile flags of a src/ object, and the
# Mbed TLS stamp it holds back (make -o) while it reads every compile line
# from a dry run.
print-compile-flags:
	@echo '$(CPPFLAGS) -Isrc $(CXXFLAGS)'
print-mbedtls-stamp:
	@echo '$(MBEDTLS_STAMP)'

# Snapshot shares the existing cache/auth/handoff; no provider in its read path.
build/price_snapshot_test: tests/price_snapshot_test.cpp $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(HOST_PTHREAD_FLAGS) $< $(filter-out build/main.o,$(OBJECTS)) $(MBEDTLS_LIB)/libmbedtls.a $(MBEDTLS_LIB)/libmbedx509.a $(MBEDTLS_LIB)/libmbedcrypto.a -o $@

# -MP only adds phony targets for headers, so a stale .d still names the old
# main source after a file moves between src/ directories. Treat such a
# missing source as out of date instead of failing: the object then rebuilds
# from its rule (whose $< is the new path) and rewrites its .d. A
# genuinely missing source still fails, in the compiler.
src/%.cpp: ;
src/%.c: ;
tests/%.cpp: ;
-include $(wildcard build/*.d)

# The node's side of the sandbox protocol. The sandbox manager itself, like
# the orchestrator, lives in https://github.com/FortisNet/node-os.
build/sandbox_ipc_test: tests/sandbox_ipc_test.cpp build/sandbox_socket.o build/bounded_json.o
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(LINK_INPUTS) -pthread -o $@
test-prereqs: build/sandbox_ipc_test

.PHONY: native-release
native-release:
	./scripts/build-native-release.sh "$(VERSION)" x86_64
	./scripts/build-native-release.sh "$(VERSION)" aarch64

.PHONY: https-unit-tests https-unit-coverage
https-unit-tests: build/https_listener_unit_test build/https_tls_unit_test build/http_transport_unit_test
	./build/https_listener_unit_test
	./build/https_tls_unit_test
	./build/http_transport_unit_test
https-unit-coverage:
	python3 scripts/https_unit_coverage.py

.PHONY: print-production-sources
print-production-sources:
	@$(foreach source,$(filter-out $(FORTIS_GUEST_ONLY_SOURCES),$(FORTIS_SOURCES)),echo $(source);)

.PHONY: check-cpp-contracts
check-cpp-contracts:
	python3 scripts/check_memory_ownership.py
	python3 scripts/cpp_contracts.py

