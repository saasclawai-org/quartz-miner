#include "quartz.h"
/**
 * quartz_entropy.h — Hardware RNG Hardening & Entropy Health Checks
 *
 * PREVENTS THE COLDCARD BUG CLASS.
 *
 * Coldcard's $130M exploit (July 2026) was caused by:
 *   - #ifndef MICROPY_HW_ENABLE_RNG checked existence, not value
 *   - Hardware TRNG silently replaced by software PRNG (Yasmarang)
 *   - ~40 bits entropy instead of 128 — keys derivable from chip serial + timer
 *   - No runtime health checks — invisible for 5 years
 *
 * Quartz prevents this through FIVE layers:
 *
 * 1. NO COMPILE-TIME RNG SWITCHES
 *    The Coldcard bug was a preprocessor logic error. Quartz has exactly
 *    ONE code path for random number generation. No #ifdef, no fallback.
 *    If the hardware RNG isn't ready, the device REFUSES to boot.
 *
 * 2. RADIO-FIRST INITIALIZATION
 *    ESP32's hardware RNG is seeded by radio noise (WiFi/BLE).
 *    esp_random() returns software-derived values before radio init.
 *    Quartz REQUIRES WiFi or BLE radio to be initialized and active
 *    for at least 100ms before any key generation occurs.
 *
 * 3. ENTROPY HEALTH CHECKS (NIST SP 800-90B)
 *    Before burning eFuse or generating keys, 1024 bytes of raw entropy
 *    are sampled and subjected to:
 *      a. Repetition count test — detect stuck values
 *      b. Adaptive proportion test — detect bias toward 0 or 1
 *      c. Chi-square test — detect non-uniform distributions
 *      d. Min-entropy estimate — must be ≥ 7.0 bits/byte
 *    If ANY test fails, key generation ABORTS. Device displays error.
 *
 * 4. TRIPLE-MIXED ENTROPY POOL
 *    Don't rely on a single entropy source. Mix three independent sources:
 *      a. esp_fill_random() — RF subsystem noise
 *      b. ADC noise — SAR ADC reading floating pin
 *      c. SRAM PUF — uninitialized SRAM at boot
 *    XOR all three together. Attacker must control ALL three to predict output.
 *
 * 5. PUBLIC VERIFIABILITY
 *    Birth certificate includes entropy_sample_hash — SHA-256 of the 1024
 *    health-check samples. Independent auditors can verify entropy quality
 *    from purchased devices.
 */

#ifndef QUARTZ_ENTROPY_H
#define QUARTZ_ENTROPY_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ============ Constants ============ */

#define QZ_ENTROPY_SAMPLE_SIZE    1024   /* Bytes sampled for health check */
#define QZ_ENTROPY_MIN_BITS       7.0    /* Min bits/byte (NIST) */
#define QZ_ENTROPY_RADIO_WARMUP_MS 100   /* Radio must be active this long */
#define QZ_ENTROPY_POOL_SIZE      32     /* Output pool size */

/* Health check result */
typedef enum {
    QZ_ENTROPY_OK = 0,
    QZ_ENTROPY_NOT_READY = 1,       /* Radio not initialized */
    QZ_ENTROPY_REPETITION_FAIL = 2, /* Stuck value detected */
    QZ_ENTROPY_PROPORTION_FAIL = 3, /* Bit bias detected */
    QZ_ENTROPY_CHISQUARE_FAIL = 4,  /* Non-uniform distribution */
    QZ_ENTROPY_MINENTROPY_FAIL = 5, /* Below 7.0 bits/byte */
    QZ_ENTROPY_SRAM_FAIL = 6,       /* SRAM PUF unavailable */
    QZ_ENTROPY_ADC_FAIL = 7,        /* ADC noise unavailable */
} qz_entropy_status_t;

/* ============ API ============ */

/**
 * Wait for hardware RNG to be fully seeded.
 *
 * ESP32's RNG is seeded by:
 *   1. WiFi/BLE radio noise (primary source)
 *   2. SAR ADC noise
 *   3. SRAM power-on state
 *
 * Before radio init, esp_random() returns software-derived values
 * that are NOT cryptographically secure. This function blocks until
 * the radio has been active for QZ_ENTROPY_RADIO_WARMUP_MS.
 *
 * This is the #1 defense against the Coldcard bug class.
 *
 * @return QZ_OK if RNG is ready, QZ_ERR_NOT_READY if timeout
 */
qz_err_t quartz_entropy_wait_for_ready(void);

/**
 * Check if hardware RNG is ready (non-blocking).
 * @return true if radio is initialized and warmed up
 */
bool quartz_entropy_is_ready(void);

/**
 * Run NIST SP 800-90B health checks on raw entropy samples.
 *
 * Tests:
 *   1. Repetition count: no value appears > 50 times consecutively
 *   2. Adaptive proportion: 0/1 balance within 45-55%
 *   3. Chi-square: Pearson's test p-value > 0.001
 *   4. Min-entropy estimate: ≥ 7.0 bits/byte
 *
 * @param samples  Buffer of QZ_ENTROPY_SAMPLE_SIZE raw bytes
 * @return QZ_ENTROPY_OK if all tests pass
 */
qz_entropy_status_t quartz_entropy_health_check(const uint8_t *samples);

/**
 * Generate cryptographically secure random bytes.
 *
 * This is the ONLY function that should be called for key generation.
 * It:
 *   1. Verifies radio is initialized (blocks if not)
 *   2. Samples from THREE sources (RF, ADC, SRAM PUF)
 *   3. XORs all sources together
 *   4. Runs health check on samples
 *   5. Returns mixed output
 *
 * If health check fails, returns QZ_ERR_FAIL and output is zeroed.
 *
 * @param output  Output buffer
 * @param len     Number of bytes (max 256)
 * @return QZ_OK on success
 */
qz_err_t quartz_secure_random(uint8_t *output, size_t len);

/**
 * Generate 32 bytes of entropy and run health check.
 * Convenience wrapper for key generation.
 *
 * @param key_out  32-byte output
 * @param sample_hash_out  SHA-256 of health-check samples (for birth cert)
 * @return QZ_OK on success
 */
qz_err_t quartz_generate_key(
    uint8_t key_out[32],
    uint8_t sample_hash_out[32]
);

/**
 * Get min-entropy estimate from last health check.
 * @return bits per byte (0.0 - 8.0)
 */
float quartz_entropy_last_estimate(void);

#endif /* QUARTZ_ENTROPY_H */
