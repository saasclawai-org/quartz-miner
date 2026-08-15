#pragma once

/*
 * Quartz Cryptography Specification
 * ---------------------------------
 * Keys:     Ed25519 (RFC 8032) — 32-byte private seed, 32-byte public key
 * Mnemonic: BIP39 12-word (128-bit entropy, SHA-256 checksum)
 * Seed:     PBKDF2-HMAC-SHA512(mnemonic, "mnemonic"+passphrase, 2048 iterations, 64 bytes)
 * HD Path:  SLIP-0010 m/44'/789'/0'/0'/0' (hardened only — Ed25519 requirement)
 * Address:  Base58(0x3B || SHA-256(pubkey)[:20] || SHA-256(SHA-256(payload))[:4])
 * Coins:    1 QZ = 100,000,000 quartz-sats (8 decimals)
 * ESP32 uses mbedTLS for SHA-256/AES-256, micro-ecc for Ed25519
 */

#include <stdint.h>
#include <stddef.h>
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include "mbedtls/aes.h"
#include "esp_mac.h"
#include "esp_random.h"

typedef int qz_err_t;

#define QZ_OK           0
#define QZ_ERR_FAIL     (-1)
#define QZ_ERR_INVALID  (-2)
#define QZ_ERR_NOT_FOUND (-3)
#define QZ_ERR_NO_MEM   (-4)
#define QZ_ERR_NO_USB   (-5)
#define QZ_ERR_CORRUPT  (-6)
#define QZ_ERR_TAMPERED (-7)
#define QZ_ERR_HARDWARE  (-8)
#define QZ_ERR_IO        (-9)
#define QZ_ERR_UNSUPPORTED (-10)
#define QZ_ERR_INVALID_SIG (-11)
#define QZ_ERR_ALREADY    (-12)


#define QUARTZ_VERSION          1
#define QUARTZ_BLOCK_TIME_SEC   120
#define QUARTZ_DIFFICULTY_BITS  20      /* starting difficulty */
#define QUARTZ_RETARGET_PERIOD  144     /* blocks between retargets */
#define QUARTZ_HALVING_INTERVAL 210000
#define QUARTZ_INITIAL_REWARD   5000000000  /* 50 QZ in quartz-sats */
#define QUARTZ_MINER_REWARD     4750000000  /* 47.5 QZ to miner (95%) */
#define QUARTZ_DEV_FUND_REWARD  250000000   /* 2.5 QZ to dev fund (5%) */
#define QUARTZ_TOTAL_SUPPLY     4200000000000000  /* 42M QZ in quartz-sats */
#define QUARTZ_DEV_FUND_TOTAL   210000000000000  /* 2.1M QZ dev fund (5%) */
#define QUARTZ_EARLY_BONUS_MINERS 1000       /* first 1000 miners get 2x */
#define QUARTZ_EARLY_BONUS_DAYS 30          /* 2x reward for first 30 days */
#define QUARTZ_MAX_TX_PER_BLOCK 255
#define QUARTZ_BLOCK_SIZE_MAX   4096
#define QUARTZ_SCRATCHPAD_SIZE  (256 * 1024)  /* 256KB (PSRAM) */
#define QUARTZ_SCRATCHPAD_SIZE_LITE  (16 * 1024)  /* 16KB fallback (no PSRAM, BLE needs RAM) */

/* --- Block header (80 bytes) --- */
#define QUARTZ_HEADER_SIZE 80

typedef struct __attribute__((packed)) {
    uint32_t version;
    uint8_t  prev_block_hash[32];
    uint8_t  merkle_root[32];
    uint32_t timestamp;
    uint32_t difficulty_target;   /* compact bits */
    uint64_t nonce;
    uint8_t  miner_id[6];         /* ESP32 MAC address */
    uint16_t padding;             /* align to 80 bytes */
} quartz_header_t;

//_Static_assert(sizeof(quartz_header_t) == QUARTZ_HEADER_SIZE, "Header must be 80 bytes");

/* --- Transaction structure --- */
#define QUARTZ_TX_VERSION 1

typedef struct __attribute__((packed)) {
    uint8_t  version;
    uint8_t  input_count;
    /* inputs:  [32 prev_hash][1 output_index][64 signature][32 pubkey] per input */
    uint8_t  output_count;
    /* outputs: [8 amount][32 script_pubkey] per output */
    uint32_t locktime;
} quartz_tx_header_t;

#define QUARTZ_TX_INPUT_SIZE  (32 + 1 + 64 + 32)   /* 129 bytes */
#define QUARTZ_TX_OUTPUT_SIZE (8 + 32)              /* 40 bytes */

/* --- CrystalHash v2 PoW (Hardware-Bound) --- */

#define CRYSTALHASH_MIXING_ROUNDS  64
#define CRYSTALHASH_HMAC_INTERVAL  8   /* HMAC injected every 8 rounds */
#define CRYSTALHASH_HMAC_CALLS     (CRYSTALHASH_MIXING_ROUNDS / CRYSTALHASH_HMAC_INTERVAL) /* 8 */

/**
 * Compute CrystalHash v2 for a block header.
 *
 * v2 interleaves the eFuse HMAC key into the hash computation itself.
 * Every 8 rounds, the hash state is mixed through HMAC-SHA256(key, state)
 * where the key lives in eFuse BLOCK6 — physically unreadable.
 *
 * This makes it impossible to compute the hash on a GPU. Each nonce
 * attempt requires 8 round-trips through the ESP32's hardware HMAC
 * engine. GPU speed becomes irrelevant; the ESP32 HMAC throughput
 * is the bottleneck.
 *
 * Attack analysis:
 *   GPU + 0 ESP32 = cannot mine (no eFuse key)
 *   GPU + 1 ESP32 = ESP32 speed (HMAC bottleneck)
 *   GPU + 10 ESP32 = 10× ESP32 speed (but 10× hardware cost)
 *   Advantage of GPU over honest ESP32 miner = ZERO
 *
 * @param header       80-byte block header
 * @param nonce        8-byte nonce
 * @param out          32-byte output hash
 * @param scratchpad   256KB scratchpad buffer (PSRAM)
 * @param use_efuse    If true, use hardware eFuse HMAC (normal operation)
 *                     If false, skip HMAC steps (verification only)
 */
void crystal_hash_v2(const uint8_t *header, uint64_t nonce,
                     uint8_t out[32], uint8_t *scratchpad, bool use_efuse);

/* Legacy v1 alias for compatibility */
#define crystal_hash(header, nonce, out, scratchpad) crystal_hash_v2(header, nonce, out, scratchpad, true)

/**
 * Check if a hash meets the difficulty target.
 */
bool quartz_check_difficulty(const uint8_t hash[32], uint32_t target_bits);

/**
 * Get the ESP32's unique miner ID (from MAC address).
 */
void quartz_get_miner_id(uint8_t miner_id[6]);

/* --- Block operations --- */

/**
 * Compute merkle root from transaction hashes.
 */
void quartz_merkle_root(const uint8_t (*tx_hashes)[32], size_t count,
                        uint8_t root[32]);

/**
 * Serialize a block header to bytes.
 */
void quartz_header_serialize(const quartz_header_t *hdr, uint8_t out[QUARTZ_HEADER_SIZE]);

/**
 * Deserialize a block header from bytes.
 */
void quartz_header_deserialize(const uint8_t *in, quartz_header_t *hdr);

/* --- Utility --- */

/**
 * Convert compact difficulty bits to a 256-bit target.
 */
void quartz_bits_to_target(uint32_t bits, uint8_t target[32]);

/**
 * Get current ESP32 clock cycles (for PUF timing).
 */
uint32_t quartz_get_cycle_count(void);

/* --- Mining Stats --- */

typedef struct {
    uint32_t hashrate;       /* Hashes per second */
    uint32_t blocks_found;   /* Total blocks mined */
    uint32_t uptime_sec;     /* Uptime in seconds */
    float    temp_c;         /* Chip temperature */
} quartz_mining_stats_t;

void quartz_mining_get_stats(quartz_mining_stats_t *stats);
