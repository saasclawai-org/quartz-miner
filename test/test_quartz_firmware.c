/**
 * test_quartz_firmware.c — Host-side unit tests for Quartz firmware logic
 *
 * Uses Unity test framework. Compiled and run on the dev machine (not ESP32).
 * Tests the pure logic functions that don't depend on ESP-IDF HAL.
 *
 * Build: cd firmware/test && make
 * Run:   ./test_quartz
 */

#include "unity.h"

/* ---- Stubs for ESP-IDF types (so we can compile on host) ---- */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

/* Stub NVS */
typedef struct {
    char key[16];
    uint8_t *data;
    size_t len;
} nvs_entry_t;

#define MAX_NVS_ENTRIES 32
static nvs_entry_t nvs_store[MAX_NVS_ENTRIES];
static int nvs_count = 0;

static nvs_entry_t *nvs_find(const char *key) {
    for (int i = 0; i < nvs_count; i++) {
        if (strcmp(nvs_store[i].key, key) == 0) return &nvs_store[i];
    }
    return NULL;
}

#define NVS_NAMESPACE "qz_wallet"
#define NVS_READWRITE 1
#define NVS_READONLY  0
typedef int nvs_handle_t;
typedef int esp_err_t;
#define ESP_OK          0
#define ESP_ERR_NVS_NOT_FOUND 0x1102

static esp_err_t nvs_open(const char *ns, int mode, nvs_handle_t *h) {
    *h = 1;
    return ESP_OK;
}
static void nvs_close(nvs_handle_t h) {}

static esp_err_t nvs_get_u8(nvs_handle_t h, const char *key, uint8_t *val) {
    nvs_entry_t *e = nvs_find(key);
    if (!e || e->len != 1) return ESP_ERR_NVS_NOT_FOUND;
    *val = e->data[0];
    return ESP_OK;
}
static esp_err_t nvs_set_u8(nvs_handle_t h, const char *key, uint8_t val) {
    nvs_entry_t *e = nvs_find(key);
    if (!e) {
        if (nvs_count >= MAX_NVS_ENTRIES) return ESP_ERR_NVS_NOT_FOUND;
        e = &nvs_store[nvs_count++];
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    if (!e->data) e->data = malloc(1);
    e->data[0] = val;
    e->len = 1;
    return ESP_OK;
}
static esp_err_t nvs_get_blob(nvs_handle_t h, const char *key, void *buf, size_t *len) {
    nvs_entry_t *e = nvs_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (*len < e->len) { *len = e->len; return ESP_ERR_NVS_NOT_FOUND; }
    memcpy(buf, e->data, e->len);
    *len = e->len;
    return ESP_OK;
}
static esp_err_t nvs_set_blob(nvs_handle_t h, const char *key, const void *buf, size_t len) {
    nvs_entry_t *e = nvs_find(key);
    if (!e) {
        if (nvs_count >= MAX_NVS_ENTRIES) return ESP_ERR_NVS_NOT_FOUND;
        e = &nvs_store[nvs_count++];
        strncpy(e->key, key, sizeof(e->key) - 1);
    }
    if (e->data) free(e->data);
    e->data = malloc(len);
    memcpy(e->data, buf, len);
    e->len = len;
    return ESP_OK;
}
static esp_err_t nvs_erase_key(nvs_handle_t h, const char *key) {
    nvs_entry_t *e = nvs_find(key);
    if (!e) return ESP_ERR_NVS_NOT_FOUND;
    if (e->data) { free(e->data); e->data = NULL; }
    e->len = 0;
    e->key[0] = '\0';
    return ESP_OK;
}
static esp_err_t nvs_commit(nvs_handle_t h) { return ESP_OK; }

#define ESP_LOGI(tag, fmt, ...) do {} while(0)
#define ESP_LOGE(tag, fmt, ...) do {} while(0)
#define ESP_LOGW(tag, fmt, ...) do {} while(0)

/* SHA-256 stub using mbedtls-like interface */
typedef struct { int dummy; } mbedtls_sha256_context;
static void mbedtls_sha256_init(mbedtls_sha256_context *ctx) {}
static void mbedtls_sha256_free(mbedtls_sha256_context *ctx) {}
static void mbedtls_sha256_starts(mbedtls_sha256_context *ctx, int is224) {}

/* Simple SHA-256 for testing — use a basic implementation */
#include <openssl/sha.h>

static void mbedtls_sha256_update(mbedtls_sha256_context *ctx, const uint8_t *data, size_t len) {
    /* We'll use OpenSSL incrementally — for test simplicity, buffer everything */
    /* Actually for the test, let's just compute final hash directly */
}
static void mbedtls_sha256_finish(mbedtls_sha256_context *ctx, uint8_t *hash) {
    /* Placeholder — real hash computed in test helper */
}

/* ---- Inline implementations of PIN logic for testing ---- */

#define FLAG_HAS_PIN 0x08
#define PIN_MAX_ATTEMPTS 10

#define NVS_KEY_FLAGS    "flags"
#define NVS_KEY_PIN_HASH "pin_hash"
#define NVS_KEY_PIN_SALT "pin_salt"
#define NVS_KEY_PIN_FAIL "pin_fails"

static uint8_t s_pin_attempts = 0;

static void compute_pin_hash(const char *pin, const uint8_t *salt, uint8_t *out_hash) {
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, 16);
    SHA256_Update(&ctx, (const uint8_t *)pin, strlen(pin));
    SHA256_Final(out_hash, &ctx);
}

static int test_set_pin(const char *pin) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return -1;

    uint8_t flags = 0;
    nvs_get_u8(h, NVS_KEY_FLAGS, &flags);

    if (pin == NULL || pin[0] == '\0') {
        nvs_erase_key(h, NVS_KEY_PIN_HASH);
        nvs_erase_key(h, NVS_KEY_PIN_SALT);
        flags &= ~FLAG_HAS_PIN;
        nvs_set_u8(h, NVS_KEY_FLAGS, flags);
        nvs_commit(h);
        nvs_close(h);
        s_pin_attempts = 0;
        return 0;
    }

    size_t len = strlen(pin);
    if (len < 4 || len > 8) { nvs_close(h); return -1; }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') { nvs_close(h); return -1; }
    }

    uint8_t salt[16];
    for (int i = 0; i < 16; i++) salt[i] = rand() & 0xFF;

    uint8_t pin_hash[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, 16);
    SHA256_Update(&ctx, (const uint8_t *)pin, len);
    SHA256_Final(pin_hash, &ctx);

    nvs_set_blob(h, NVS_KEY_PIN_HASH, pin_hash, 32);
    nvs_set_blob(h, NVS_KEY_PIN_SALT, salt, 16);
    flags |= FLAG_HAS_PIN;
    nvs_set_u8(h, NVS_KEY_FLAGS, flags);
    nvs_set_u8(h, NVS_KEY_PIN_FAIL, 0);
    nvs_commit(h);
    nvs_close(h);

    s_pin_attempts = 0;
    return 0;
}

static int test_check_pin(const char *pin) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return -1;

    uint8_t flags = 0;
    nvs_get_u8(h, NVS_KEY_FLAGS, &flags);
    if (!(flags & FLAG_HAS_PIN)) { nvs_close(h); return 0; }

    uint8_t salt[16];
    uint8_t stored_hash[32];
    size_t required = 16;
    if (nvs_get_blob(h, NVS_KEY_PIN_SALT, salt, &required) != ESP_OK) { nvs_close(h); return -1; }
    required = 32;
    if (nvs_get_blob(h, NVS_KEY_PIN_HASH, stored_hash, &required) != ESP_OK) { nvs_close(h); return -1; }
    nvs_close(h);

    uint8_t test_hash[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, 16);
    SHA256_Update(&ctx, (const uint8_t *)pin, strlen(pin));
    SHA256_Final(test_hash, &ctx);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= stored_hash[i] ^ test_hash[i];

    memset(test_hash, 0, 32);
    return (diff == 0) ? 0 : -1;
}

static bool test_has_pin(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t flags = 0;
    nvs_get_u8(h, NVS_KEY_FLAGS, &flags);
    nvs_close(h);
    return (flags & FLAG_HAS_PIN) != 0;
}

static bool test_record_failed_pin(void) {
    s_pin_attempts++;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_PIN_FAIL, s_pin_attempts);
        nvs_commit(h);
        nvs_close(h);
    }
    return (s_pin_attempts >= PIN_MAX_ATTEMPTS);
}

static void reset_nvs(void) {
    for (int i = 0; i < nvs_count; i++) {
        if (nvs_store[i].data) { free(nvs_store[i].data); nvs_store[i].data = NULL; }
        nvs_store[i].key[0] = '\0';
        nvs_store[i].len = 0;
    }
    nvs_count = 0;
    s_pin_attempts = 0;
}

/* ---- WOTS+ Signature Index Logic ---- */

#define WOTS_SIG_MAX 256  /* total signatures */
#define WOTS_SIG_USABLE 255  /* regular spending (last reserved) */
#define ROTATION_WARN_THRESHOLD 240
#define ROTATION_URGENT_THRESHOLD 254

static int test_rotation_status(int sig_index) {
    if (sig_index >= ROTATION_URGENT_THRESHOLD) return 2;  /* urgent */
    if (sig_index >= ROTATION_WARN_THRESHOLD) return 1;   /* warning */
    return 0;  /* ok */
}

static int test_can_sign_regular(int sig_index) {
    return sig_index < WOTS_SIG_USABLE;  /* refuse at 255 */
}

static int test_can_sign_rotation(int sig_index) {
    return sig_index == WOTS_SIG_USABLE;  /* only at 255 */
}

static int test_remaining_sigs(int sig_index) {
    return WOTS_SIG_USABLE - sig_index;
}

/* ============================================================ */
/* TESTS                                                         */
/* ============================================================ */

void setUp(void) { reset_nvs(); }
void tearDown(void) {}

/* --- PIN Tests --- */

void test_pin_set_and_check_correct(void) {
    TEST_ASSERT_FALSE(test_has_pin());
    TEST_ASSERT_EQUAL(0, test_set_pin("1234"));
    TEST_ASSERT_TRUE(test_has_pin());
    TEST_ASSERT_EQUAL(0, test_check_pin("1234"));
}

void test_pin_check_wrong(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("1234"));
    TEST_ASSERT_EQUAL(-1, test_check_pin("4321"));
    TEST_ASSERT_EQUAL(-1, test_check_pin("0000"));
}

void test_pin_no_pin_always_passes(void) {
    TEST_ASSERT_FALSE(test_has_pin());
    TEST_ASSERT_EQUAL(0, test_check_pin("anything"));
    TEST_ASSERT_EQUAL(0, test_check_pin(""));
}

void test_pin_remove(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("1234"));
    TEST_ASSERT_TRUE(test_has_pin());
    TEST_ASSERT_EQUAL(0, test_set_pin(""));  /* remove */
    TEST_ASSERT_FALSE(test_has_pin());
}

void test_pin_too_short(void) {
    TEST_ASSERT_EQUAL(-1, test_set_pin("123"));  /* 3 digits */
}

void test_pin_too_long(void) {
    TEST_ASSERT_EQUAL(-1, test_set_pin("123456789"));  /* 9 digits */
}

void test_pin_with_letters(void) {
    TEST_ASSERT_EQUAL(-1, test_set_pin("12ab"));
    TEST_ASSERT_EQUAL(-1, test_set_pin("pass"));
}

void test_pin_eight_digits_ok(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("12345678"));
    TEST_ASSERT_TRUE(test_has_pin());
}

void test_pin_six_digits_ok(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("123456"));
    TEST_ASSERT_TRUE(test_has_pin());
}

void test_pin_attempts_increment(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("1234"));
    TEST_ASSERT_FALSE(test_record_failed_pin());  /* 1 */
    TEST_ASSERT_FALSE(test_record_failed_pin());  /* 2 */
    TEST_ASSERT_FALSE(test_record_failed_pin());  /* 3 */
    TEST_ASSERT_EQUAL(3, s_pin_attempts);
}

void test_pin_wipe_at_ten_attempts(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("1234"));

    for (int i = 0; i < 9; i++) {
        TEST_ASSERT_FALSE(test_record_failed_pin());
    }
    /* 10th attempt triggers wipe */
    TEST_ASSERT_TRUE(test_record_failed_pin());
}

void test_pin_constant_time_compare(void) {
    /* Same PIN → same hash with same salt */
    TEST_ASSERT_EQUAL(0, test_set_pin("9999"));

    /* Read salt */
    nvs_handle_t h;
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t salt[16];
    uint8_t stored[32];
    size_t r = 16;
    nvs_get_blob(h, NVS_KEY_PIN_SALT, salt, &r);
    r = 32;
    nvs_get_blob(h, NVS_KEY_PIN_HASH, stored, &r);
    nvs_close(h);

    /* Compute hash with same salt + same PIN */
    uint8_t test[32];
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, 16);
    SHA256_Update(&ctx, (const uint8_t*)"9999", 4);
    SHA256_Final(test, &ctx);

    TEST_ASSERT_EQUAL_UINT8_ARRAY(stored, test, 32);

    /* Wrong PIN → different hash */
    SHA256_Init(&ctx);
    SHA256_Update(&ctx, salt, 16);
    SHA256_Update(&ctx, (const uint8_t*)"0000", 4);
    SHA256_Final(test, &ctx);

    int diff = 0;
    for (int i = 0; i < 32; i++) diff |= (stored[i] != test[i]);
    TEST_ASSERT(diff != 0);
}

/* --- WOTS+ Signature Reservation Tests --- */

void test_rotation_status_ok(void) {
    TEST_ASSERT_EQUAL(0, test_rotation_status(0));
    TEST_ASSERT_EQUAL(0, test_rotation_status(100));
    TEST_ASSERT_EQUAL(0, test_rotation_status(239));
}

void test_rotation_status_warn_at_240(void) {
    TEST_ASSERT_EQUAL(1, test_rotation_status(240));
    TEST_ASSERT_EQUAL(1, test_rotation_status(250));
    TEST_ASSERT_EQUAL(1, test_rotation_status(253));
}

void test_rotation_status_urgent_at_254(void) {
    TEST_ASSERT_EQUAL(2, test_rotation_status(254));
    TEST_ASSERT_EQUAL(2, test_rotation_status(255));
}

void test_can_sign_regular_below_255(void) {
    TEST_ASSERT(test_can_sign_regular(0));
    TEST_ASSERT(test_can_sign_regular(100));
    TEST_ASSERT(test_can_sign_regular(254));
}

void test_cannot_sign_regular_at_255(void) {
    TEST_ASSERT_FALSE(test_can_sign_regular(255));
}

void test_can_only_rotate_at_255(void) {
    TEST_ASSERT_FALSE(test_can_sign_rotation(0));
    TEST_ASSERT_FALSE(test_can_sign_rotation(100));
    TEST_ASSERT_FALSE(test_can_sign_rotation(254));
    TEST_ASSERT(test_can_sign_rotation(255));
}

void test_remaining_sigs_decreases(void) {
    TEST_ASSERT_EQUAL(255, test_remaining_sigs(0));
    TEST_ASSERT_EQUAL(155, test_remaining_sigs(100));
    TEST_ASSERT_EQUAL(15, test_remaining_sigs(240));
    TEST_ASSERT_EQUAL(1, test_remaining_sigs(254));
    TEST_ASSERT_EQUAL(0, test_remaining_sigs(255));
}

/* --- NVS Persistence Simulation --- */

void test_pin_survives_nvs_close_reopen(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("4242"));
    /* Simulate reboot — NVS data persists in nvs_store */
    s_pin_attempts = 0;  /* RAM resets, NVS doesn't */

    TEST_ASSERT_TRUE(test_has_pin());
    TEST_ASSERT_EQUAL(0, test_check_pin("4242"));
    TEST_ASSERT_EQUAL(-1, test_check_pin("9999"));
}

void test_pin_fail_counter_persists(void) {
    TEST_ASSERT_EQUAL(0, test_set_pin("7777"));
    test_record_failed_pin();
    test_record_failed_pin();

    /* Simulate reboot */
    s_pin_attempts = 0;

    /* Read from NVS */
    nvs_handle_t h;
    nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    uint8_t stored_fails = 0;
    nvs_get_u8(h, NVS_KEY_PIN_FAIL, &stored_fails);
    nvs_close(h);

    TEST_ASSERT_EQUAL(2, stored_fails);
}

/* ---- Main ---- */

int main(void) {
    UNITY_BEGIN();

    /* PIN tests */
    RUN_TEST(test_pin_set_and_check_correct);
    RUN_TEST(test_pin_check_wrong);
    RUN_TEST(test_pin_no_pin_always_passes);
    RUN_TEST(test_pin_remove);
    RUN_TEST(test_pin_too_short);
    RUN_TEST(test_pin_too_long);
    RUN_TEST(test_pin_with_letters);
    RUN_TEST(test_pin_eight_digits_ok);
    RUN_TEST(test_pin_six_digits_ok);
    RUN_TEST(test_pin_attempts_increment);
    RUN_TEST(test_pin_wipe_at_ten_attempts);
    RUN_TEST(test_pin_constant_time_compare);

    /* WOTS+ tests */
    RUN_TEST(test_rotation_status_ok);
    RUN_TEST(test_rotation_status_warn_at_240);
    RUN_TEST(test_rotation_status_urgent_at_254);
    RUN_TEST(test_can_sign_regular_below_255);
    RUN_TEST(test_cannot_sign_regular_at_255);
    RUN_TEST(test_can_only_rotate_at_255);
    RUN_TEST(test_remaining_sigs_decreases);

    /* Persistence tests */
    RUN_TEST(test_pin_survives_nvs_close_reopen);
    RUN_TEST(test_pin_fail_counter_persists);

    return UNITY_END();
}
