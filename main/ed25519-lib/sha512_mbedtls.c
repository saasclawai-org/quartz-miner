/*
 * SHA-512 wrapper using mbedtls — replaces bundled LibTomCrypt SHA-512
 * Same API as orlp/ed25519 sha512.h
 */
#include "sha512.h"
#include "mbedtls/sha512.h"
#include <string.h>

void sha512_init(sha512_context *ctx) {
    ctx->md_info = malloc(sizeof(mbedtls_sha512_context));
    mbedtls_sha512_init((mbedtls_sha512_context *)ctx->md_info);
    mbedtls_sha512_starts((mbedtls_sha512_context *)ctx->md_info, 0);
}

void sha512_update(sha512_context *ctx, const unsigned char *data, size_t len) {
    mbedtls_sha512_update((mbedtls_sha512_context *)ctx->md_info, data, len);
}

void sha512_final(sha512_context *ctx, unsigned char *digest) {
    mbedtls_sha512_finish((mbedtls_sha512_context *)ctx->md_info, digest);
    mbedtls_sha512_free((mbedtls_sha512_context *)ctx->md_info);
    free(ctx->md_info);
    ctx->md_info = NULL;
}

void sha512(const unsigned char *data, size_t len, unsigned char *digest) {
    mbedtls_sha512(data, len, digest, 0);
}
