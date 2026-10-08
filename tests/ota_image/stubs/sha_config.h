#pragma once

/* Native SHA-256 uses the pinned SDK's software implementation. */
#define MBEDTLS_SHA256_C
#define MBEDTLS_PLATFORM_ZEROIZE_ALT
