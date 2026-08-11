#ifndef SHA512_H
#define SHA512_H
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

typedef struct {
    void *md_info;
} sha512_context;

void sha512_init(sha512_context *ctx);
void sha512_update(sha512_context *ctx, const unsigned char *data, size_t len);
void sha512_final(sha512_context *ctx, unsigned char *digest);
void sha512(const unsigned char *data, size_t len, unsigned char *digest);

#endif
