/**
 * quartz_bip39.c — Standard BIP39 + SLIP-0010 Ed25519 key derivation
 *
 * Replaces the old one-way privkey→mnemonic encoding with a proper
 * two-way BIP39 pipeline so the same 12 words produce the same key
 * in any wallet (web, firmware, hardware).
 *
 * Pipeline:
 *   1. 16 bytes entropy → 12-word BIP39 mnemonic (with checksum)
 *   2. mnemonic → PBKDF2-HMAC-SHA512("mnemonic", 2048 iters) → 64-byte seed
 *   3. HMAC-SHA512("ed25519 seed", seed) → master_key[32] || chain_code[32]
 *   4. BIP44 path m/44'/789'/0'/0'/0' via SLIP-0010 hardened derivation
 *   5. final 32-byte key → Ed25519 keypair
 *
 * Matches the web wallet's deriveQuartzKeypair() exactly.
 */

#include "quartz_wallet.h"
#include "quartz.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#endif

#include "bip39_wordlist.h"

static const char *TAG = "QZ.BIP39";

/* ============================================================
 * BIP39: entropy → mnemonic words
 * ============================================================ */

void quartz_entropy_to_mnemonic(const uint8_t entropy[16],
                                 char words[12][12],
                                 size_t max_word_len) {
    /* Checksum = first 4 bits of SHA-256(entropy) */
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

/* ============================================================
 * BIP39: mnemonic → 64-byte seed (PBKDF2-HMAC-SHA512)
 * ============================================================ */

static void quartz_mnemonic_to_seed(const char words[12][12],
                                     uint8_t seed[64]) {
    /* Join words with spaces: "word1 word2 ... word12" */
    char mnemonic_str[12 * 12 + 12];  /* 12 words * max 11 chars + 11 spaces + NUL */
    mnemonic_str[0] = '\0';
    for (int i = 0; i < 12; i++) {
        if (i > 0) strcat(mnemonic_str, " ");
        strcat(mnemonic_str, words[i]);
    }

    /* Salt = "mnemonic" (no passphrase) */
    const char *salt = "mnemonic";
    size_t salt_len = 8;

#ifdef ESP_PLATFORM
    /* PBKDF2-HMAC-SHA512, 2048 iterations, 64 bytes output */
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA512,
        (const unsigned char *)mnemonic_str, strlen(mnemonic_str),
        (const unsigned char *)salt, salt_len,
        2048, 64, seed);
#else
    /* Non-ESP placeholder */
    memset(seed, 0, 64);
#endif
}

/* ============================================================
 * HMAC-SHA512 (for SLIP-0010 master key + child derivation)
 * ============================================================ */

static void hmac_sha512(const uint8_t *key, size_t key_len,
                         const uint8_t *data, size_t data_len,
                         uint8_t out[64]) {
#ifdef ESP_PLATFORM
    /* Use mbedtls MD HMAC */
    mbedtls_md_context_t ctx;
    const mbedtls_md_info_t *md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, md_info, 1);  /* 1 = HMAC */
    mbedtls_md_hmac_starts(&ctx, key, key_len);
    mbedtls_md_hmac_update(&ctx, data, data_len);
    mbedtls_md_hmac_finish(&ctx, out);
    mbedtls_md_free(&ctx);
#else
    memset(out, 0, 64);
#endif
}

/* ============================================================
 * SLIP-0010 + BIP44: seed → Ed25519 private key
 *
 * Path: m/44'/789'/0'/0'/0'
 * All levels hardened (SLIP-0010 for Ed25519 requires hardened
 * derivation at every level).
 * ============================================================ */

void quartz_bip39_derive_key(const char words[12][12],
                              uint8_t privkey[32],
                              uint8_t pubkey[32]) {
    /* Step 1: mnemonic → 64-byte seed */
    uint8_t seed[64];
    quartz_mnemonic_to_seed(words, seed);

    /* Step 2: master key = HMAC-SHA512("ed25519 seed", seed) */
    uint8_t master[64];
    hmac_sha512((const uint8_t *)"ed25519 seed", 14, seed, 64, master);
    /* master[0:32] = key, master[32:64] = chain code */

    uint8_t key[32];
    uint8_t chain[32];
    memcpy(key, master, 32);
    memcpy(chain, master + 32, 32);

    /* Step 3: derive m/44'/789'/0'/0'/0' */
    uint32_t path[] = { 44 | 0x80000000, 789 | 0x80000000,
                        0  | 0x80000000, 0  | 0x80000000,
                        0  | 0x80000000 };
    int path_len = 5;

    for (int i = 0; i < path_len; i++) {
        /* SLIP-0010 hardened child derivation:
         * data = 0x00 || key || uint32BE(index)
         * I = HMAC-SHA512(chain, data)
         * child_key = I[0:32], child_chain = I[32:64] */
        uint8_t data[37];
        data[0] = 0x00;
        memcpy(data + 1, key, 32);
        data[33] = (path[i] >> 24) & 0xFF;
        data[34] = (path[i] >> 16) & 0xFF;
        data[35] = (path[i] >> 8)  & 0xFF;
        data[36] = path[i]         & 0xFF;

        uint8_t i_out[64];
        hmac_sha512(chain, 32, data, 37, i_out);
        memcpy(key, i_out, 32);
        memcpy(chain, i_out + 32, 32);
    }

    /* Final 32-byte key is the Ed25519 seed */
    memcpy(privkey, key, 32);

    /* Derive pubkey */
    quartz_ed25519_keypair(privkey, pubkey);

    /* Wipe intermediates */
    memset(seed, 0, sizeof(seed));
    memset(master, 0, sizeof(master));
    memset(key, 0, sizeof(key));
    memset(chain, 0, sizeof(chain));
}

/* ============================================================
 * Validate a 12-word mnemonic (wordlist membership + checksum)
 * ============================================================ */
bool quartz_bip39_validate_words(const char words[12][12]) {
    uint8_t bits[132];
    int n = 0;

    for (int w = 0; w < 12; w++) {
        int idx = -1;
        for (int i = 0; i < 2048; i++) {
            if (strcmp(bip39_wordlist[i], words[w]) == 0) { idx = i; break; }
        }
        if (idx < 0) return false;  /* word not in list */
        for (int bit = 10; bit >= 0; bit--) bits[n++] = (idx >> bit) & 1;
    }

    uint8_t entropy[16];
    for (int i = 0; i < 16; i++) {
        uint8_t v = 0;
        for (int k = 0; k < 8; k++) v = (v << 1) | bits[i * 8 + k];
        entropy[i] = v;
    }

    uint8_t hash[32];
#ifdef ESP_PLATFORM
    mbedtls_sha256(entropy, 16, hash, 0);
#else
    memset(hash, 0, 32);
#endif

    uint8_t cs = 0;
    for (int k = 0; k < 4; k++) cs = (cs << 1) | bits[128 + k];
    return cs == (hash[0] >> 4);
}
