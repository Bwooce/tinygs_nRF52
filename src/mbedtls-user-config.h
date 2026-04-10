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

#ifndef MBEDTLS_PKCS1_V21
#define MBEDTLS_PKCS1_V21
#endif

/* Force legacy (non-PSA) RSA verify path in pk_wrap.c.
 * The PSA drivers (CC310/Oberon) return FEATURE_UNAVAILABLE for
 * PKCS1v15 RSA signature verification during TLS handshake. */
#ifdef MBEDTLS_USE_PSA_CRYPTO
#undef MBEDTLS_USE_PSA_CRYPTO
#endif

/* Force ECDHE-RSA key exchange — nrf-config.h #undef's this because
 * the NCS Kconfig path doesn't resolve it with OpenThread PSA backend.
 * mqtt.tinygs.com requires ECDHE-RSA-AES256-GCM-SHA384. */
#ifndef MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#endif


/* SHA384 requires SHA512 in mbedTLS */
#ifndef MBEDTLS_SHA512_C
#define MBEDTLS_SHA512_C
#endif

/* PEM parsing for CA cert */
#ifndef MBEDTLS_PEM_PARSE_C
#define MBEDTLS_PEM_PARSE_C
#endif

#ifndef MBEDTLS_BASE64_C
#define MBEDTLS_BASE64_C
#endif

/* X.509 cert parsing needed for server cert verification during handshake */
#ifndef MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRT_PARSE_C
#endif

#ifndef MBEDTLS_X509_USE_C
#define MBEDTLS_X509_USE_C
#endif

#ifndef MBEDTLS_OID_C
#define MBEDTLS_OID_C
#endif

#ifndef MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_PARSE_C
#endif

/* PK (public key) parsing for server certificate validation */
#ifndef MBEDTLS_PK_C
#define MBEDTLS_PK_C
#endif

#ifndef MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_PARSE_C
#endif

/* Debug: find which feature is unavailable during TLS handshake */
#ifndef MBEDTLS_DEBUG_C
#define MBEDTLS_DEBUG_C
#endif
