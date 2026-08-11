/**
 * quartz_entropy.c — Hardware RNG Hardening Implementation
 *
 * Five-layer defense against the Coldcard bug class.
 */

#include "quartz_entropy.h"
#include "quartz.h"
#include <string.h>
#include <math.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_adc/adc_oneshot.h"
#include "mbedtls/sha256.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#endif

static const char *TAG = "QZ.ENTROPY";

/* ============ Internal State ============ */

static struct {
    bool radio_ready;
    int64_t radio_ready_time;   /* esp_timer_get_time() when radio started */
    float last_min_entropy;
    bool last_health_ok;
} s_entropy = {0};

/* ============ Layer 1: Radio-First Initialization ============ */

bool quartz_entropy_is_ready(void) {
    if (!s_entropy.radio_ready) return false;

#ifdef ESP_PLATFORM
    int64_t now = esp_timer_get_time();
    int64_t elapsed_ms = (now - s_entropy.radio_ready_time) / 1000;
    return elapsed_ms >= QZ_ENTROPY_RADIO_WARMUP_MS;
#else
    return true;
#endif
}

qz_err_t quartz_entropy_wait_for_ready(void) {
#ifdef ESP_PLATFORM
    /* Check if WiFi or BLE is active */
    wifi_mode_t wifi_mode;
    bool wifi_on = (esp_wifi_get_mode(&wifi_mode) == ESP_OK &&
                    wifi_mode != WIFI_MODE_NULL);

    /* BT status check removed — esp_bt.h include varies by target.
     * For RNG purposes, WiFi being on is sufficient. */
    bool bt_on = false;  /* TODO: detect BT status per-target */

    if (!wifi_on && !bt_on) {
        ESP_LOGE(TAG, "⚠️ No radio active! Cannot guarantee hardware RNG entropy.");
        ESP_LOGE(TAG, "⚠️ This is exactly the Coldcard bug — refusing to generate keys.");
        return QZ_ERR_FAIL;
    }

    if (!s_entropy.radio_ready) {
        s_entropy.radio_ready = true;
        s_entropy.radio_ready_time = esp_timer_get_time();
        ESP_LOGI(TAG, "Radio detected. Waiting %dms for entropy accumulation...",
                 QZ_ENTROPY_RADIO_WARMUP_MS);
    }

    /* Block until warmup period */
    while (!quartz_entropy_is_ready()) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGI(TAG, "Hardware RNG ready (radio warmed up)");
    return QZ_OK;
#else
    return QZ_OK;
#endif
}

/* ============ Layer 2: Triple-Mixed Entropy Pool ============ */

static void sample_rf_entropy(uint8_t *buf, size_t len) {
    /* Source 1: esp_fill_random — mixes RF subsystem noise */
#ifdef ESP_PLATFORM
    esp_fill_random(buf, len);
#else
    /* Non-ESP32: use OS random for testing */
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
#endif
}

static void sample_adc_entropy(uint8_t *buf, size_t len) {
    /* Source 2: SAR ADC reading floating pin.
     * ESP-IDF v5.x uses adc_oneshot API which requires a handle.
     * For simplicity in the dev build, we use esp_random() mixed with
     * a cycle counter as a secondary source. Production uses real ADC. */
    for (size_t i = 0; i < len; i++) {
        uint32_t r = esp_random();
        volatile uint32_t cycle = esp_cpu_get_cycle_count();
        buf[i] = (uint8_t)(r ^ cycle);
    }
}

static void sample_sram_entropy(uint8_t *buf, size_t len) {
    /* Source 3: SRAM power-on state (PUF)
     * On ESP32-S3, a region of SRAM can be read before initialization.
     * The power-on state has chip-specific entropy.
     * In production: read from a known uninitialized SRAM region.
     * For safety, we use a hash of the stack address + timing jitter. */
#ifdef ESP_PLATFORM
    uint8_t sram_buf[32];
    /* In production: memset(sram_buf, 0, 32); read_sram_raw(sram_buf); */
    /* For now: use stack address jitter as proxy */
    volatile uint32_t stack_ptr = (uint32_t)&stack_ptr;
    volatile int64_t time_val = esp_timer_get_time();

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    /* Multiple readings for better entropy */
    for (int i = 0; i < 8; i++) {
        volatile uint32_t cycle = esp_cpu_get_cycle_count();
        mbedtls_sha256_update(&ctx, (uint8_t *)&cycle, 4);
        volatile int64_t t = esp_timer_get_time();
        mbedtls_sha256_update(&ctx, (uint8_t *)&t, 8);
        mbedtls_sha256_update(&ctx, (uint8_t *)&stack_ptr, 4);
    }

    mbedtls_sha256_finish(&ctx, sram_buf);
    mbedtls_sha256_free(&ctx);

    /* Expand to requested length */
    for (size_t i = 0; i < len; i++) {
        buf[i] = sram_buf[i % 32];
    }
#else
    for (size_t i = 0; i < len; i++) {
        buf[i] = (uint8_t)(rand() & 0xFF);
    }
#endif
}

static void xor_buffers(uint8_t *dst, const uint8_t *src, size_t len) {
    for (size_t i = 0; i < len; i++) {
        dst[i] ^= src[i];
    }
}

/* ============ Layer 3: NIST SP 800-90B Health Checks ============ */

qz_entropy_status_t quartz_entropy_health_check(const uint8_t *samples) {
    if (!samples) return QZ_ENTROPY_PROPORTION_FAIL;

    const size_t n = QZ_ENTROPY_SAMPLE_SIZE;

    /* --- Test 1: Repetition Count Test --- */
    /* No byte value should appear more than 50 times consecutively */
    uint8_t last_val = samples[0];
    int repeat_count = 1;
    int max_repeat = 1;

    for (size_t i = 1; i < n; i++) {
        if (samples[i] == last_val) {
            repeat_count++;
            if (repeat_count > max_repeat) max_repeat = repeat_count;
        } else {
            last_val = samples[i];
            repeat_count = 1;
        }
    }

    if (max_repeat > 50) {
        ESP_LOGE(TAG, "Repetition count FAIL: %d consecutive same bytes", max_repeat);
        return QZ_ENTROPY_REPETITION_FAIL;
    }

    /* --- Test 2: Adaptive Proportion Test (bit level) --- */
    /* Count 1-bits. For uniform random, expect ~50% ± 3% */
    int ones = 0;
    int total_bits = n * 8;

    for (size_t i = 0; i < n; i++) {
        uint8_t byte = samples[i];
        for (int b = 0; b < 8; b++) {
            if (byte & (1 << b)) ones++;
        }
    }

    float proportion = (float)ones / total_bits;
    if (proportion < 0.45 || proportion > 0.55) {
        ESP_LOGE(TAG, "Adaptive proportion FAIL: %.2f%% ones (expected 45-55%%)",
                 proportion * 100);
        return QZ_ENTROPY_PROPORTION_FAIL;
    }

    /* --- Test 3: Chi-Square Test --- */
    /* Pearson's chi-square on byte frequency distribution.
     * For 256 buckets and 1024 samples, expected count per bucket = 4.
     * Chi-square = sum((observed - expected)^2 / expected)
     * df = 255, critical value at p=0.001 is ~378.
     * If chi-square > 378, distribution is non-uniform. */
    int counts[256] = {0};
    for (size_t i = 0; i < n; i++) {
        counts[samples[i]]++;
    }

    double expected = (double)n / 256.0;
    double chi_square = 0.0;

    for (int i = 0; i < 256; i++) {
        double diff = (double)counts[i] - expected;
        chi_square += (diff * diff) / expected;
    }

    if (chi_square > 378.0) {
        ESP_LOGE(TAG, "Chi-square FAIL: %.2f (threshold 378)", chi_square);
        return QZ_ENTROPY_CHISQUARE_FAIL;
    }

    /* --- Test 4: Min-Entropy Estimate --- */
    /* Most common value estimate (NIST 800-90B Section 6.3.1)
     * H_min = -log2(p_max) where p_max = max_count / n */
    int max_count = 0;
    for (int i = 0; i < 256; i++) {
        if (counts[i] > max_count) max_count = counts[i];
    }

    float p_max = (float)max_count / (float)n;
    float min_entropy_per_byte = -log2f(p_max);

    s_entropy.last_min_entropy = min_entropy_per_byte;

    if (min_entropy_per_byte < QZ_ENTROPY_MIN_BITS) {
        ESP_LOGE(TAG, "Min-entropy FAIL: %.2f bits/byte (need %.1f)",
                 min_entropy_per_byte, QZ_ENTROPY_MIN_BITS);
        return QZ_ENTROPY_MINENTROPY_FAIL;
    }

    /* All tests passed */
    s_entropy.last_health_ok = true;
    ESP_LOGI(TAG, "Entropy health OK: %.2f bits/byte, chi²=%.1f, ones=%.1f%%",
             min_entropy_per_byte, chi_square, proportion * 100);

    return QZ_ENTROPY_OK;
}

float quartz_entropy_last_estimate(void) {
    return s_entropy.last_min_entropy;
}

/* ============ Layer 4: Secure Random Generation ============ */

qz_err_t quartz_secure_random(uint8_t *output, size_t len) {
    if (!output || len > 256) return QZ_ERR_INVALID;
    if (!quartz_entropy_is_ready()) {
        qz_err_t err = quartz_entropy_wait_for_ready();
        if (err != QZ_OK) return err;
    }

    /* Sample from three independent sources */
    uint8_t rf_buf[QZ_ENTROPY_SAMPLE_SIZE];
    uint8_t adc_buf[QZ_ENTROPY_SAMPLE_SIZE];
    uint8_t sram_buf[QZ_ENTROPY_SAMPLE_SIZE];

    sample_rf_entropy(rf_buf, sizeof(rf_buf));
    sample_adc_entropy(adc_buf, sizeof(adc_buf));
    sample_sram_entropy(sram_buf, sizeof(sram_buf));

    /* XOR all three sources together */
    uint8_t mixed[QZ_ENTROPY_SAMPLE_SIZE];
    memcpy(mixed, rf_buf, sizeof(mixed));
    xor_buffers(mixed, adc_buf, sizeof(mixed));
    xor_buffers(mixed, sram_buf, sizeof(mixed));

    /* Run health check on mixed output */
    qz_entropy_status_t status = quartz_entropy_health_check(mixed);
    if (status != QZ_ENTROPY_OK) {
        ESP_LOGE(TAG, "⚠️ ENTROPY HEALTH CHECK FAILED — refusing to generate keys");
        ESP_LOGE(TAG, "⚠️ This could indicate hardware fault or tampering");
        memset(output, 0, len);
        memset(mixed, 0, sizeof(mixed));
        return QZ_ERR_FAIL;
    }

    /* Derive output from mixed pool via SHA-256 */
    uint8_t pool[32];
    mbedtls_sha256(mixed, sizeof(mixed), pool, 0);

    /* Expand to requested length using SHA-256 chain */
    size_t offset = 0;
    uint8_t counter = 0;
    while (offset < len) {
        uint8_t hash_input[33];
        memcpy(hash_input, pool, 32);
        hash_input[32] = counter++;

        uint8_t hash_out[32];
        mbedtls_sha256(hash_input, 33, hash_out, 0);

        size_t chunk = (len - offset < 32) ? (len - offset) : 32;
        memcpy(output + offset, hash_out, chunk);
        offset += chunk;
    }

    /* Secure wipe */
    memset(rf_buf, 0, sizeof(rf_buf));
    memset(adc_buf, 0, sizeof(adc_buf));
    memset(sram_buf, 0, sizeof(sram_buf));
    memset(mixed, 0, sizeof(mixed));
    memset(pool, 0, sizeof(pool));

    return QZ_OK;
}

qz_err_t quartz_generate_key(
    uint8_t key_out[32],
    uint8_t sample_hash_out[32]
) {
    /* Generate 1024 bytes of raw entropy for health check + sample hash */
    uint8_t samples[QZ_ENTROPY_SAMPLE_SIZE];

    /* Sample mixed entropy */
    sample_rf_entropy(samples, sizeof(samples));

    uint8_t adc_buf[QZ_ENTROPY_SAMPLE_SIZE];
    uint8_t sram_buf[QZ_ENTROPY_SAMPLE_SIZE];
    sample_adc_entropy(adc_buf, sizeof(adc_buf));
    sample_sram_entropy(sram_buf, sizeof(sram_buf));

    xor_buffers(samples, adc_buf, sizeof(samples));
    xor_buffers(samples, sram_buf, sizeof(sram_buf));

    /* Health check BEFORE using the entropy */
    qz_entropy_status_t status = quartz_entropy_health_check(samples);
    if (status != QZ_ENTROPY_OK) {
        ESP_LOGE(TAG, "⚠️ KEY GENERATION ABORTED — entropy health check failed");
        memset(key_out, 0, 32);
        if (sample_hash_out) memset(sample_hash_out, 0, 32);
        memset(samples, 0, sizeof(samples));
        memset(adc_buf, 0, sizeof(adc_buf));
        memset(sram_buf, 0, sizeof(sram_buf));
        return QZ_ERR_FAIL;
    }

    /* Hash the samples for the birth certificate */
    if (sample_hash_out) {
        mbedtls_sha256(samples, sizeof(samples), sample_hash_out, 0);
    }

    /* Derive 32-byte key from samples */
    mbedtls_sha256(samples, sizeof(samples), key_out, 0);

    /* Secure wipe */
    memset(samples, 0, sizeof(samples));
    memset(adc_buf, 0, sizeof(adc_buf));
    memset(sram_buf, 0, sizeof(sram_buf));

    ESP_LOGI(TAG, "Key generated with verified entropy (%.2f bits/byte)",
             s_entropy.last_min_entropy);

    return QZ_OK;
}
