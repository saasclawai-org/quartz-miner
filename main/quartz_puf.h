/**
 * quartz_puf.h — SRAM PUF + Fuzzy Extractor for Hardware-Bound Mining
 *
 * Uses ESP32 SRAM power-on state as a physically unclonable function.
 * A fuzzy extractor (BCH-lite + helper data) produces a stable,
 * reproducible key from the noisy SRAM readings.
 *
 * Mining protocol: each block includes a PUF challenge-response
 * that can only be produced by the physical ESP32 chip.
 */

#ifndef QUARTZ_PUF_H
#define QUARTZ_PUF_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PUF-derived key size (256 bits = 32 bytes) */
#define QZ_PUF_KEY_SIZE     32
#define QZ_PUF_SRAM_SAMPLES 256   /* bytes of SRAM to sample */
#define QZ_PUF_ENROLL_ROUNDS 5    /* samples during enrollment */

/* Helper data stored in NVS (XOR mask + stability map) */
typedef struct {
    uint8_t  helper_mask[QZ_PUF_KEY_SIZE];    /* XOR(puf_bits, enrolled_key) */
    uint8_t  stability_map[QZ_PUF_KEY_SIZE];  /* which bits are stable */
    uint8_t  enrolled_hash[QZ_PUF_KEY_SIZE];  /* SHA256 of enrolled key (verification) */
    uint8_t  challenge_salt[16];              /* salt for mining challenges */
    bool     enrolled;                        /* has PUF been enrolled? */
} qz_puf_helper_t;

/* PUF state */
typedef enum {
    QZ_PUF_UNENROLLED = 0,
    QZ_PUF_ENROLLED,
    QZ_PUF_ERROR,
} qz_puf_state_t;

/**
 * Initialize PUF subsystem.
 * On first call: enrolls (samples SRAM multiple times, builds helper data).
 * On subsequent calls: reconstructs key from SRAM + helper data.
 *
 * Must be called VERY early in boot, before SRAM is fully initialized
 * by the runtime. In practice, called from app_main before heap init.
 */
int quartz_puf_init(void);

/**
 * Get the current PUF-derived key (32 bytes).
 * Returns NULL if PUF not initialized.
 */
const uint8_t *quartz_puf_get_key(void);

/**
 * Generate a mining challenge-response for a given block header.
 *
 * challenge = SHA256(header + puf_key + nonce + salt)
 *
 * This can ONLY be produced by this specific ESP32 chip.
 * A GPU miner cannot reproduce this without the physical SRAM state.
 */
void quartz_puf_mining_response(
    const uint8_t *header,     /* 80-byte block header */
    uint64_t nonce,            /* mining nonce */
    uint8_t response[32]       /* output: 32-byte PUF response */
);

/**
 * Check if PUF is enrolled and stable.
 */
qz_puf_state_t quartz_puf_get_state(void);

/**
 * Get a hex string of the PUF key fingerprint (for display).
 * First 16 hex chars of SHA256(key).
 */
const char *quartz_puf_get_fingerprint(void);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_PUF_H
