/*
 * mbedTLS user config overrides for TinyGS nRF52
 *
 * The NCS PSA crypto path doesn't enable legacy MBEDTLS_RSA_C and
 * MBEDTLS_PKCS1_V15 via Kconfig. But the TLS key exchange code
 * (ECDHE-RSA) needs these defines to compile non-empty cipher suites.
 * mqtt.tinygs.com requires ECDHE-RSA-AES256-GCM-SHA384 over TLS 1.2.
 */

#ifndef MBEDTLS_RSA_C
#define MBEDTLS_RSA_C
#endif

#ifndef MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V15
#endif
