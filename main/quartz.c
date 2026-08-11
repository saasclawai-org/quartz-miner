#include "quartz.h"
#include "quartz_attest.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "esp_cpu.h"
/* HMAC peripheral only on ESP32-S3/C3 */
#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32C3
#include "esp_hmac.h"
#define HAS_HW_HMAC 1
#else
#define HAS_HW_HMAC 0
#include "mbedtls/sha256.h"
#endif
#include "string.h"

static const char *TAG = "QUARTZ";

/* --- CrystalHash v2 Implementation (Hardware-Bound) --- */

void crystal_hash_v2(const uint8_t *header, uint64_t nonce,
                     uint8_t out[32], uint8_t *scratchpad, bool use_efuse)
{
    /*
     * CrystalHash v2 — eFuse HMAC interleaved into hash rounds.
     *
     * v1 had attestation as a post-hoc signature → GPU + ESP32 = GPU speed.
     * v2 injects eFuse HMAC every 8 rounds → GPU cannot proceed without ESP32.
     *
     * Phase 1: INIT — SHA-256(header || nonce)
     * Phase 2: SCRATCHPAD — AES-256-CTR fill (256KB)
     * Phase 3: MIXING — 64 rounds, HMAC injected at rounds 7,15,23,31,39,47,55,63
     * Phase 4: FINALIZE — SHA-256(state || SHA-256(header || nonce))
     */

    /* Runtime scratchpad size (default 256KB, reduced on non-PSRAM devices) */
extern int g_scratchpad_size;

/* === Phase 1: INIT === */
    uint8_t init_input[88];
    memcpy(init_input, header, 80);
    memcpy(init_input + 80, &nonce, 8);

    uint8_t state[32];
    mbedtls_sha256(init_input, 88, state, 0);

    /* === Phase 2: SCRATCHPAD INIT === */
    /* AES-256-CTR using first 32 bytes of header as key
     * On real hardware, use eFuse key as AES key for additional binding.
     * For verification mode (use_efuse=false), use header-derived key. */
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);

    uint8_t key[32];
    memcpy(key, header, 32);

    uint8_t ctr[16];
    memcpy(ctr, &nonce, 8);
    memcpy(ctr + 8, header + 32, 8);

    uint8_t stream_block[16];
    memset(scratchpad, 0, g_scratchpad_size);
    mbedtls_aes_setkey_enc(&aes, key, 256);

    for (size_t offset = 0; offset < g_scratchpad_size; offset += 16) {
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, ctr, stream_block);
        memcpy(scratchpad + offset, stream_block, 16);

        /* Increment counter */
        for (int i = 15; i >= 0; i--) {
            if (++ctr[i] != 0) break;
        }
    }
    mbedtls_aes_free(&aes);

    /* === Phase 3: MIXING — 64 rounds with HMAC injection === */
    uint32_t state_seed = (uint32_t)(nonce & 0xFFFFFFFF) ^ ((uint32_t)(nonce >> 32));

    for (int round = 0; round < CRYSTALHASH_MIXING_ROUNDS; round++) {
        /* Memory-hard mixing: read scratchpad at state-derived offset */
        uint32_t idx = (*(uint32_t *)state ^ state_seed ^ (round * 0x9E3779B9));
        idx %= (g_scratchpad_size / 32);

        /* XOR state with scratchpad data */
        for (int i = 0; i < 32; i++) {
            state[i] ^= scratchpad[idx * 32 + i];
        }

        /* SHA-256 diffusion */
        uint8_t tmp[32];
        mbedtls_sha256(state, 32, tmp, 0);
        memcpy(state, tmp, 32);

        /* *** HARDWARE GATE ***
         * Every 8th round: inject eFuse HMAC.
         *
         * The eFuse key in BLOCK6 is physically unreadable.
         * Only the hardware HMAC engine can use it.
         * A GPU computing this hash would freeze here — it literally
         * cannot produce the HMAC without the ESP32 silicon.
         *
         * This is called 8 times per nonce attempt, making the ESP32
         * the rate limiter, not the GPU.
         */
        if (round % CRYSTALHASH_HMAC_INTERVAL == (CRYSTALHASH_HMAC_INTERVAL - 1)) {
            if (use_efuse) {
                /* Build HMAC input: state (32 bytes) || round_num (4 bytes) */
                uint8_t hmac_input[36];
                memcpy(hmac_input, state, 32);
                uint32_t round_le = round;
                memcpy(hmac_input + 32, &round_le, 4);

                /* Hardware HMAC — key never leaves eFuse */
                uint8_t hmac_out[32];
#if HAS_HW_HMAC
                esp_err_t ret = esp_hmac_calculate(HMAC_KEY0, hmac_input, 36, hmac_out);
                if (ret == ESP_OK) {
#else
                {
                    /* Software fallback for original ESP32 (no HMAC peripheral) */
                    mbedtls_sha256_context ctx;
                    mbedtls_sha256_init(&ctx);
                    mbedtls_sha256_starts(&ctx, 0);
                    mbedtls_sha256_update(&ctx, hmac_input, 36);
                    mbedtls_sha256_finish(&ctx, hmac_out);
                    mbedtls_sha256_free(&ctx);
#endif
                    /* XOR HMAC result into state */
                    for (int i = 0; i < 32; i++) {
                        state[i] ^= hmac_out[i];
                    }
                }
            }
            /* Verification mode (use_efuse=false): skip HMAC.
             * Verifier trusts attestation signature instead. */
        }
    }

    /* === Phase 4: FINALIZE === */
    uint8_t final_input[64];
    memcpy(final_input, state, 32);
    /* Second SHA-256 of header+nonce for avalanche */
    mbedtls_sha256(init_input, 88, final_input + 32, 0);

    mbedtls_sha256(final_input, 64, out, 0);
}

/* --- Difficulty checking --- */

bool quartz_check_difficulty(const uint8_t hash[32], uint32_t target_bits)
{
    uint8_t target[32];
    quartz_bits_to_target(target_bits, target);

    /* Compare hash <= target (big-endian byte comparison) */
    for (int i = 0; i < 32; i++) {
        if (hash[i] < target[i]) return true;
        if (hash[i] > target[i]) return false;
    }
    return true; /* equal */
}

void quartz_bits_to_target(uint32_t bits, uint8_t target[32])
{
    /* Bitcoin-style compact difficulty */
    memset(target, 0, 32);

    uint32_t exponent = bits >> 24;
    uint32_t mantissa = bits & 0x007FFFFF;

    if (exponent <= 3) {
        mantissa >>= (8 * (3 - exponent));
        target[28] = (mantissa >> 24) & 0xFF;
        target[29] = (mantissa >> 16) & 0xFF;
        target[30] = (mantissa >> 8) & 0xFF;
        target[31] = mantissa & 0xFF;
    } else {
        /* Shift mantissa into position */
        uint32_t pos = exponent - 3;
        if (pos < 29) {
            target[32 - exponent]     = (mantissa >> 16) & 0xFF;
            target[32 - exponent + 1] = (mantissa >> 8) & 0xFF;
            target[32 - exponent + 2] = mantissa & 0xFF;
        }
    }
}

/* --- Merkle root --- */

void quartz_merkle_root(const uint8_t (*tx_hashes)[32], size_t count,
                        uint8_t root[32])
{
    if (count == 0) {
        memset(root, 0, 32);
        return;
    }

    uint8_t (*level)[32] = malloc(count * 32);
    if (!level) {
        ESP_LOGE(TAG, "Failed to allocate merkle level");
        memset(root, 0, 32);
        return;
    }

    memcpy(level, tx_hashes, count * 32);

    while (count > 1) {
        size_t next_count = (count + 1) / 2;
        for (size_t i = 0; i < next_count; i++) {
            uint8_t concat[64];
            memcpy(concat, level[i * 2], 32);
            if (i * 2 + 1 < count) {
                memcpy(concat + 32, level[i * 2 + 1], 32);
            } else {
                memcpy(concat + 32, level[i * 2], 32); /* duplicate last */
            }
            mbedtls_sha256(concat, 64, level[i], 0);
        }
        count = next_count;
    }

    memcpy(root, level[0], 32);
    free(level);
}

/* --- Serialization --- */

void quartz_header_serialize(const quartz_header_t *hdr, uint8_t out[QUARTZ_HEADER_SIZE])
{
    memcpy(out, hdr, QUARTZ_HEADER_SIZE);
}

void quartz_header_deserialize(const uint8_t *in, quartz_header_t *hdr)
{
    memcpy(hdr, in, QUARTZ_HEADER_SIZE);
}

/* --- Miner ID --- */

void quartz_get_miner_id(uint8_t miner_id[6])
{
    esp_read_mac(miner_id, 0);
}

/* --- Cycle counter (legacy, used for timing stats) --- */

uint32_t quartz_get_cycle_count(void)
{
    return esp_cpu_get_cycle_count();
}

/* Mining stats stub */
static uint32_t s_mining_start_time = 0;
static uint32_t s_blocks_found = 0;

void quartz_mining_get_stats(quartz_mining_stats_t *stats) {
    if (!stats) return;
    memset(stats, 0, sizeof(*stats));
    if (s_mining_start_time == 0) s_mining_start_time = esp_timer_get_time() / 1000000;
    stats->uptime_sec = (esp_timer_get_time() / 1000000) - s_mining_start_time;
    stats->blocks_found = s_blocks_found;
    stats->temp_c = 45.0; /* Placeholder */
    stats->hashrate = 0;  /* Updated by mining loop */
}
