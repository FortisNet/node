// Mbed TLS user configuration (MBEDTLS_USER_CONFIG_FILE), applied after the
// pinned 3.6.7 default config by mbedtls/build_info.h. The Makefile passes
// it to the vendored library build and to every project object, and a guest
// build must pass it to every guest C and C++ object, so the library and
// its callers always agree on one configuration.
#ifndef FORTIS_MBEDTLS_CONFIG_H
#define FORTIS_MBEDTLS_CONFIG_H

// The runtime never calls the mbedtls_*_self_test functions. With them on,
// ecp.c also keeps unsynchronized global EC operation counters that every
// concurrent signature or handshake writes.
#undef MBEDTLS_SELF_TEST

// TLS sessions, X.509 and PSA crypto run on several worker threads. The
// build flags enable pthread threading; refuse an object built without it,
// whose context layouts would not match the library.
#if !defined(MBEDTLS_THREADING_C) || !defined(MBEDTLS_THREADING_PTHREAD)
#error "Fortis builds Mbed TLS with MBEDTLS_THREADING_C and MBEDTLS_THREADING_PTHREAD"
#endif

#endif  // FORTIS_MBEDTLS_CONFIG_H
