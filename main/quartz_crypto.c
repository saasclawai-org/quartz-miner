/**
 * quartz_crypto.c — Real Ed25519 + BIP39 for Quartz wallet
 *
 * Uses orlp/ed25519 (public domain) with mbedtls SHA-512.
 */

#include "quartz_wallet.h"
#include "quartz.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "mbedtls/sha256.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#endif

/* orlp/ed25519 library */
#include "ed25519-lib/ed25519.h"

static const char *TAG = "QZ.CRYPTO";

/* ============================================================
 * Real Ed25519 keypair derivation
 *
 * seed (32 bytes) → pubkey (32 bytes)
 * The private key is the seed itself (orlp convention).
 * ============================================================ */

void quartz_ed25519_keypair(const uint8_t seed[32], uint8_t pubkey[32]) {
    /* orlp ed25519 stores private_key as 64 bytes (seed || pubkey).
     * We only need the pubkey output here. */
    uint8_t expanded_priv[64];
    ed25519_create_keypair(pubkey, expanded_priv, seed);
    /* expanded_priv = seed(32) + pubkey(32), not needed — we store seed only */
}

/* ============================================================
 * Real Ed25519 signing
 *
 * Uses the orlp convention: private_key = 64-byte expanded key.
 * We reconstruct it from seed + pubkey on each sign call.
 * ============================================================ */

void quartz_ed25519_sign(const uint8_t privkey_seed[32],
                          const uint8_t *msg, size_t msg_len,
                          uint8_t signature[64]) {
    /* Reconstruct the 64-byte expanded private key (seed || pubkey) */
    uint8_t pubkey[32];
    uint8_t expanded[64];

    ed25519_create_keypair(pubkey, expanded, privkey_seed);

    /* Sign with orlp ed25519 */
    ed25519_sign(signature, msg, msg_len, pubkey, expanded);
}

/* ============================================================
 * Ed25519 signature verification (for node-side verification
 * or checking incoming transactions)
 * ============================================================ */

int quartz_ed25519_verify(const uint8_t signature[64],
                           const uint8_t *msg, size_t msg_len,
                           const uint8_t pubkey[32]) {
    return ed25519_verify(signature, msg, msg_len, pubkey);
}

/* ============================================================
 * BIP39 Mnemonic — 12-word seed phrase
 *
 * Uses first 128 bits (16 bytes) of privkey + 4-bit checksum.
 * Standard BIP39 for 12-word mnemonics.
 * ============================================================ */

#include "bip39_wordlist.h"

void quartz_privkey_to_mnemonic(const uint8_t privkey[32],
                                 char words[12][12],
                                 size_t max_word_len) {
    uint8_t entropy[16];
    memcpy(entropy, privkey, 16);

    /* Checksum = first byte of SHA-256(entropy) */
    uint8_t hash[32];
#ifdef ESP_PLATFORM
    mbedtls_sha256(entropy, 16, hash, 0);
#else
    memset(hash, 0, 32);
#endif

    /* 132 bits = 16 bytes entropy + 4 bits checksum */
    uint8_t data[17];
    memcpy(data, entropy, 16);
    data[16] = hash[0];

    for (int i = 0; i < 12; i++) {
        int bit_offset = i * 11;
        int byte_idx = bit_offset / 8;
        int bit_idx = bit_offset % 8;

        uint16_t index;
        if (bit_idx <= 5) {
            uint16_t val = ((uint16_t)data[byte_idx] << 8);
            if (byte_idx + 1 < 17) val |= data[byte_idx + 1];
            index = (val >> (16 - 11 - bit_idx)) & 0x7FF;
        } else {
            uint32_t val = ((uint32_t)data[byte_idx] << 16);
            if (byte_idx + 1 < 17) val |= ((uint32_t)data[byte_idx + 1] << 8);
            if (byte_idx + 2 < 17) val |= data[byte_idx + 2];
            index = (val >> (24 - 11 - bit_idx)) & 0x7FF;
        }

        if (index >= 2048) index %= 2048;

        strncpy(words[i], bip39_wordlist[index], max_word_len - 1);
        words[i][max_word_len - 1] = '\0';
    }
}
