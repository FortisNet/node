#pragma once

// The test-hook environment: the FORTIS_ENABLE_*TEST* switches and the
// FORTIS_TEST_* inputs read by the server's test probes and test wiring
// (tests/server/test_*). The replay binaries' main() installs the process
// environment lookup here once, at startup, and every test hook reads
// through TestHookEnv. Lookups happen when the hook asks (a probe clock
// re-reads its fixture file path per call). Test binaries only: the
// production binary carries neither this unit nor any of the variable names
// (scripts/check_production_binary.py checks its bytes).

// Installs the lookup (main passes ProcessEnvironment). Call once, before
// any TestHookEnv read.
void LoadTestHookEnv(const char* (*lookup)(void* context, const char* name), void* context);

// The value of `name` through the installed lookup (nullptr when unset or
// when no lookup is installed).
const char* TestHookEnv(const char* name);
