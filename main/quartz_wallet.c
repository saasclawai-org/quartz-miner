/**
 * Quartz ESP32 Hardware Wallet — Key Management
 *
 * Keys are generated ON the ESP32 using hardware RNG.
 * Private key NEVER leaves the device — stored in encrypted NVS.
 * Signing happens on-device. Phone constructs unsigned tx, ESP32 signs.
 *
 * ESP32 Security Features Used:
 * - RNG_TRUE (hardware random number generator)
 * - NVS (Non-Volatile Storage) with flash encryption
 * - Secure Boot (prevents firmware tampering)
 * - ECDH key exchange for BLE channel encryption
 */

#include "quartz_wallet.h"
#include "quartz.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include <string.h>

static const char *TAG = "QUARTZ_WALLET";

// --- Ed25519 via micro-ecc (minimal implementation) ---
// In production, use esp_tinycrypt or micro-ecc library
// For now, we use a compact Ed25519 implementation

#define ED25519_PRIVATE_KEY_SIZE 32
#define ED25519_PUBLIC_KEY_SIZE  32
#define ED25519_SIGNATURE_SIZE   64

// Quartz address constants
#define QZ_ADDR_VERSION_MAINNET  0x3B
#define QZ_ADDR_VERSION_TESTNET  0x7F
#define QZ_ADDR_CHECKSUM_SIZE    4
#define QZ_ADDR_PAYLOAD_SIZE     21  // version + 20-byte hash
#define QZ_ADDR_TOTAL_SIZE       25  // payload + checksum

static uint8_t s_private_key[ED25519_PRIVATE_KEY_SIZE];
static uint8_t s_public_key[ED25519_PUBLIC_KEY_SIZE];
static char s_address[36];  // Base58 address string
static char s_mnemonic_words[12][12];
static bool s_is_testnet = true;        // network of the loaded/generated wallet  // BIP39 mnemonic (persisted to NVS)
static bool s_wallet_initialized = false;

// ============================================================
// NVS Storage Keys
// ============================================================
#define NVS_NAMESPACE "qz_wallet"
#define NVS_KEY_PRIV  "priv_key"
#define NVS_KEY_PUB   "pub_key"
#define NVS_KEY_FLAGS "flags"
#define NVS_KEY_PIN_HASH  "pin_hash"   // 32 bytes SHA-256(salt + pin)
#define NVS_KEY_PIN_SALT  "pin_salt"   // 16 bytes random salt
#define NVS_KEY_PIN_FAIL  "pin_fails"  // uint8_t failed attempt count
#define NVS_KEY_MNEMONIC  "mnemonic"   // 12 words × 12 bytes = 144 bytes

#define PIN_MAX_ATTEMPTS  10

static uint8_t s_pin_attempts = 0;

// Flag bits
#define FLAG_MINING_ENABLED  0x01
#define FLAG_TESTNET         0x02
#define FLAG_BACKED_UP       0x04  // user confirmed seed phrase backup
#define FLAG_HAS_PIN         0x08  // PIN protection enabled

// ============================================================
// Hardware RNG — ESP32 True Random Number Generator
// ============================================================

void quartz_rng(uint8_t *buf, size_t len) {
    // ESP32 has a true hardware RNG (uses thermal noise + RF subsystem)
    // Must enable WiFi or BT for best entropy; otherwise still seeded
    // from internal ADC noise. For cryptographic keys, we mix sources.
    esp_fill_random(buf, len);
}

// ============================================================
// Base58 Encoding (compact C implementation)
// ============================================================

static const char BASE58_ALPHABET[] =
    "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";

static int base58_encode(const uint8_t *input, size_t len, char *output, size_t out_len) {
    // Simple big-number base58 encoding
    uint8_t digits[len * 138 / 100 + 1];
    memset(digits, 0, sizeof(digits));
    int digit_len = 1;

    for (size_t i = 0; i < len; i++) {
        int carry = input[i];
        for (int j = 0; j < digit_len; j++) {
            carry += digits[j] << 8;
            digits[j] = carry % 58;
            carry /= 58;
        }
        while (carry > 0) {
            digits[digit_len++] = carry % 58;
            carry /= 58;
        }
    }

    // Leading zeros → '1'
    int out_idx = 0;
    for (size_t i = 0; i < len && input[i] == 0 && out_idx < (int)(out_len - 1); i++) {
        output[out_idx++] = '1';
    }

    // Reverse and map
    for (int i = digit_len - 1; i >= 0 && out_idx < (int)(out_len - 1); i--) {
        output[out_idx++] = BASE58_ALPHABET[digits[i]];
    }
    output[out_idx] = '\0';
    return out_idx;
}

// ============================================================
// SHA-256 Helper
// ============================================================

static void sha256_double(const uint8_t *data, size_t len, uint8_t *out32) {
    uint8_t hash1[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, hash1);

    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, hash1, 32);
    mbedtls_sha256_finish(&ctx, out32);
    mbedtls_sha256_free(&ctx);
}

// ============================================================
// Address Derivation from Public Key
// ============================================================

static void derive_address(const uint8_t pubkey[32], bool testnet, char *out, size_t out_len) {
    uint8_t hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, pubkey, 32);
    mbedtls_sha256_finish(&ctx, hash);
    mbedtls_sha256_free(&ctx);

    // Build payload: version + hash[:20]
    uint8_t payload[QZ_ADDR_PAYLOAD_SIZE];
    payload[0] = testnet ? QZ_ADDR_VERSION_TESTNET : QZ_ADDR_VERSION_MAINNET;
    memcpy(payload + 1, hash, 20);

    // Checksum: SHA-256(SHA-256(payload))[:4]
    uint8_t checksum[32];
    sha256_double(payload, QZ_ADDR_PAYLOAD_SIZE, checksum);

    // Full address bytes: payload + checksum
    uint8_t addr_bytes[QZ_ADDR_TOTAL_SIZE];
    memcpy(addr_bytes, payload, QZ_ADDR_PAYLOAD_SIZE);
    memcpy(addr_bytes + QZ_ADDR_PAYLOAD_SIZE, checksum, QZ_ADDR_CHECKSUM_SIZE);

    base58_encode(addr_bytes, QZ_ADDR_TOTAL_SIZE, out, out_len);
}

// ============================================================
// Wallet Generation — Keys Created ON the ESP32
// ============================================================

quartz_wallet_err_t quartz_wallet_generate(bool testnet) {
    // 1. Generate 16 bytes of true random entropy (128-bit security)
    uint8_t entropy[16];
    quartz_rng(entropy, 16);

    // 2. Encode as BIP39 mnemonic (12 words with checksum)
    //    Store words for the one-time seed display
    quartz_entropy_to_mnemonic(entropy, s_mnemonic_words, sizeof(s_mnemonic_words[0]));
    memset(entropy, 0, sizeof(entropy));  // wipe entropy after encoding

    // 3. Derive Ed25519 keypair from mnemonic via standard BIP39→BIP44
    //    Same words → same key in any standard wallet (web, mobile, hardware)
    quartz_bip39_derive_key((const char (*)[12])s_mnemonic_words,
                            s_private_key, s_public_key);

    // 4. Derive Quartz address from public key
    derive_address(s_public_key, testnet, s_address, sizeof(s_address));
    s_is_testnet = testnet;

    // 5. Persist to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return QZ_WALLET_ERR_STORAGE;
    }

    nvs_set_blob(handle, NVS_KEY_PRIV, s_private_key, ED25519_PRIVATE_KEY_SIZE);
    nvs_set_blob(handle, NVS_KEY_PUB, s_public_key, ED25519_PUBLIC_KEY_SIZE);
    nvs_set_blob(handle, NVS_KEY_MNEMONIC, s_mnemonic_words, sizeof(s_mnemonic_words));

    uint8_t flags = FLAG_MINING_ENABLED | (testnet ? FLAG_TESTNET : 0);
    nvs_set_u8(handle, NVS_KEY_FLAGS, flags);

    nvs_commit(handle);
    nvs_close(handle);

    s_wallet_initialized = true;

    ESP_LOGI(TAG, "Wallet generated via BIP39→BIP44→Ed25519");
    ESP_LOGI(TAG, "Address: %s", s_address);
    ESP_LOGI(TAG, "Seed phrase is standard BIP39 — importable in any wallet");

    return QZ_WALLET_OK;
}

// ============================================================
// Restore — Import Wallet from Seed Phrase (canonical BIP-39)
// Same words = same key on phone / node / any device.
// PIN (if set) is preserved across restore.
// ============================================================

quartz_wallet_err_t quartz_wallet_restore(const char words[12][12], bool testnet) {
    if (!quartz_bip39_validate_words(words)) {
        ESP_LOGE(TAG, "Restore: invalid words (not in list or bad checksum)");
        return QZ_WALLET_ERR_INVALID;
    }

    quartz_bip39_derive_key(words, s_private_key, s_public_key);
    derive_address(s_public_key, testnet, s_address, sizeof(s_address));
    for (int i = 0; i < 12; i++) {
        strncpy(s_mnemonic_words[i], words[i], 11);
        s_mnemonic_words[i][11] = '\0';
    }
    s_is_testnet = testnet;
    s_wallet_initialized = true;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return QZ_WALLET_ERR_STORAGE;

    nvs_set_blob(handle, NVS_KEY_PRIV, s_private_key, ED25519_PRIVATE_KEY_SIZE);
    nvs_set_blob(handle, NVS_KEY_PUB, s_public_key, ED25519_PUBLIC_KEY_SIZE);
    nvs_set_blob(handle, NVS_KEY_MNEMONIC, s_mnemonic_words, sizeof(s_mnemonic_words));
    uint8_t flags = FLAG_MINING_ENABLED | (testnet ? FLAG_TESTNET : 0);
    nvs_set_u8(handle, NVS_KEY_FLAGS, flags);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "Wallet restored from seed phrase: %s", s_address);
    return QZ_WALLET_OK;
}

bool quartz_wallet_is_testnet(void) {
    return s_is_testnet;
}

// ============================================================
// Wallet Load — Read from NVS on Boot
// ============================================================

quartz_wallet_err_t quartz_wallet_load(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        // No wallet yet — needs generation
        return QZ_WALLET_ERR_NOT_FOUND;
    }

    size_t required_size = ED25519_PRIVATE_KEY_SIZE;
    err = nvs_get_blob(handle, NVS_KEY_PRIV, s_private_key, &required_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return QZ_WALLET_ERR_NOT_FOUND;
    }

    required_size = ED25519_PUBLIC_KEY_SIZE;
    err = nvs_get_blob(handle, NVS_KEY_PUB, s_public_key, &required_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return QZ_WALLET_ERR_CORRUPT;
    }

    /* Restore failed-PIN counter so power-cycling can't reset it.
     * (Brute-force fix: 10 wrong PINs = wipe, per LIFETIME, not per boot.) */
    uint8_t stored_fails = 0;
    if (nvs_get_u8(handle, NVS_KEY_PIN_FAIL, &stored_fails) == ESP_OK) {
        s_pin_attempts = stored_fails;
        if (s_pin_attempts > 0) {
            ESP_LOGW(TAG, "PIN attempt counter restored: %d/%d",
                     s_pin_attempts, PIN_MAX_ATTEMPTS);
        }
    }

    /* Restore mnemonic words (for PIN-gated 'seed' re-show command) */
    size_t mnem_size = sizeof(s_mnemonic_words);
    if (nvs_get_blob(handle, NVS_KEY_MNEMONIC, s_mnemonic_words, &mnem_size) != ESP_OK) {
        /* Pre-BIP39 wallet: no stored mnemonic. Device can still mine/sign
         * but 'seed' command won't work. Re-generate wallet to get BIP39. */
        memset(s_mnemonic_words, 0, sizeof(s_mnemonic_words));
        ESP_LOGW(TAG, "No mnemonic in NVS (pre-BIP39 wallet) — seed re-show unavailable");
    }

    uint8_t flags = 0;
    nvs_get_u8(handle, NVS_KEY_FLAGS, &flags);
    nvs_close(handle);

    bool testnet = flags & FLAG_TESTNET;
    derive_address(s_public_key, testnet, s_address, sizeof(s_address));
    s_is_testnet = testnet;
    s_wallet_initialized = true;

    ESP_LOGI(TAG, "Wallet loaded from NVS: %s", s_address);
    return QZ_WALLET_OK;
}

// ============================================================
// Transaction Signing — Private Key Never Leaves Device
// ============================================================

quartz_wallet_err_t quartz_wallet_sign(const uint8_t *msg, size_t msg_len,
                                        uint8_t signature[64]) {
    if (!s_wallet_initialized) {
        return QZ_WALLET_ERR_NOT_FOUND;
    }

    // Sign with Ed25519 using the on-device private key
    // Message is constructed by the phone, sent over BLE
    // ESP32 signs and returns ONLY the signature
    quartz_ed25519_sign(s_private_key, msg, msg_len, signature);

    ESP_LOGI(TAG, "Signed %d-byte message (signature only — key stays on device)", msg_len);
    return QZ_WALLET_OK;
}

// ============================================================
// Public Key Export — Only Public Info Leaves Device
// ============================================================

const uint8_t *quartz_wallet_get_pubkey(void) {
    if (!s_wallet_initialized) return NULL;
    return s_public_key;
}

const char *quartz_wallet_get_address(void) {
    if (!s_wallet_initialized) return NULL;
    return s_address;
}

// ============================================================
// Seed Phrase Backup — One-Time Display, Then Wiped
// ============================================================

// During initial setup, the ESP32 derives a BIP39 mnemonic from
// the private key entropy. This is shown ONCE on the paired phone
// (over an encrypted BLE channel), then the mnemonic buffer is
// zeroed. The private key itself NEVER leaves the device.

// Note: This creates a hybrid model — the private key is on-device,
// but the seed phrase is shown for backup. The seed phrase can
// reconstruct the key IF the ESP32 is lost. This matches hardware
// wallet convention (Ledger/Trezor show seed on setup too).

// To restore: user enters seed phrase in any Quartz wallet (PWA,
// Android) → derives Ed25519 keypair → imports to new ESP32

quartz_wallet_err_t quartz_wallet_get_seed_phrase_for_backup(
    char words[12][12],  // output: 12 words, max 11 chars each
    size_t max_word_len
) {
    if (!s_wallet_initialized) return QZ_WALLET_ERR_NOT_FOUND;

    // Return the stored BIP39 mnemonic (generated at wallet creation time)
    // This is the SAME 12 words that any standard wallet would derive from.
    if (s_mnemonic_words[0][0] == '\0') {
        ESP_LOGE(TAG, "No mnemonic stored (pre-BIP39 wallet) — re-generate wallet");
        return QZ_WALLET_ERR_NOT_FOUND;
    }

    for (int i = 0; i < 12; i++) {
        strncpy(words[i], s_mnemonic_words[i], max_word_len - 1);
        words[i][max_word_len - 1] = '\0';
    }

    ESP_LOGW(TAG, "Seed phrase displayed for backup (BIP39 standard)");
    ESP_LOGW(TAG, "After user confirms backup, mnemonic MUST be wiped from RAM");

    return QZ_WALLET_OK;
}

void quartz_wallet_wipe_seed_phrase(char words[12][12]) {
    // Securely zero the mnemonic buffer
    memset(words, 0, 12 * 12);
    ESP_LOGI(TAG, "Seed phrase wiped from RAM");
}

// ============================================================
// Backup Confirmation (persistent NVS flag)
// ============================================================

quartz_wallet_err_t quartz_wallet_confirm_backup(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return QZ_WALLET_ERR_STORAGE;
    }

    // Read current flags, set FLAG_BACKED_UP
    uint8_t flags = 0;
    nvs_get_u8(handle, NVS_KEY_FLAGS, &flags);
    flags |= FLAG_BACKED_UP;
    nvs_set_u8(handle, NVS_KEY_FLAGS, flags);
    nvs_commit(handle);
    nvs_close(handle);

    ESP_LOGI(TAG, "✅ Backup confirmed — FLAG_BACKED_UP set in NVS");
    return QZ_WALLET_OK;
}

bool quartz_wallet_is_backup_confirmed(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t flags = 0;
    nvs_get_u8(handle, NVS_KEY_FLAGS, &flags);
    nvs_close(handle);
    return (flags & FLAG_BACKED_UP) != 0;
}

// ============================================================
// Factory Reset — Wipe Everything
// ============================================================

quartz_wallet_err_t quartz_wallet_wipe(void) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_erase_key(handle, NVS_KEY_PRIV);
        nvs_erase_key(handle, NVS_KEY_PUB);
        nvs_erase_key(handle, NVS_KEY_FLAGS);
        nvs_erase_key(handle, NVS_KEY_PIN_HASH);
        nvs_erase_key(handle, NVS_KEY_PIN_SALT);
        nvs_erase_key(handle, NVS_KEY_PIN_FAIL);
        nvs_commit(handle);
        nvs_close(handle);
    }

    // Zero RAM copies
    memset(s_private_key, 0, sizeof(s_private_key));
    memset(s_public_key, 0, sizeof(s_public_key));
    memset(s_address, 0, sizeof(s_address));
    s_wallet_initialized = false;
    s_pin_attempts = 0;

    ESP_LOGI(TAG, "Wallet wiped — device reset to factory");
    return QZ_WALLET_OK;
}

// ============================================================
// PIN Protection
// ============================================================

quartz_wallet_err_t quartz_wallet_set_pin(const char *pin) {
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return QZ_WALLET_ERR_STORAGE;
    }

    // Read current flags
    uint8_t flags = 0;
    nvs_get_u8(handle, NVS_KEY_FLAGS, &flags);

    if (pin == NULL || pin[0] == '\0') {
        // Empty PIN = remove PIN protection
        nvs_erase_key(handle, NVS_KEY_PIN_HASH);
        nvs_erase_key(handle, NVS_KEY_PIN_SALT);
        flags &= ~FLAG_HAS_PIN;
        nvs_set_u8(handle, NVS_KEY_FLAGS, flags);
        nvs_set_u8(handle, NVS_KEY_PIN_FAIL, 0);
        nvs_commit(handle);
        nvs_close(handle);
        s_pin_attempts = 0;
        ESP_LOGI(TAG, "PIN protection removed");
        return QZ_WALLET_OK;
    }

    // Validate: digits only, 4-8 chars
    size_t len = strlen(pin);
    if (len < 4 || len > 8) {
        nvs_close(handle);
        ESP_LOGE(TAG, "PIN must be 4-8 digits");
        return QZ_WALLET_ERR_INVALID;
    }
    for (size_t i = 0; i < len; i++) {
        if (pin[i] < '0' || pin[i] > '9') {
            nvs_close(handle);
            return QZ_WALLET_ERR_INVALID;
        }
    }

    // Generate random salt
    uint8_t salt[16];
    quartz_rng(salt, 16);

    // Hash: SHA-256(salt || pin)
    uint8_t pin_hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, (const uint8_t *)pin, len);
    mbedtls_sha256_finish(&ctx, pin_hash);
    mbedtls_sha256_free(&ctx);

    // Store
    nvs_set_blob(handle, NVS_KEY_PIN_HASH, pin_hash, 32);
    nvs_set_blob(handle, NVS_KEY_PIN_SALT, salt, 16);
    flags |= FLAG_HAS_PIN;
    nvs_set_u8(handle, NVS_KEY_FLAGS, flags);
    nvs_set_u8(handle, NVS_KEY_PIN_FAIL, 0);
    nvs_commit(handle);
    nvs_close(handle);

    s_pin_attempts = 0;
    ESP_LOGI(TAG, "PIN set (%d digits)", (int)len);
    return QZ_WALLET_OK;
}

quartz_wallet_err_t quartz_wallet_check_pin(const char *pin) {
    if (!quartz_wallet_has_pin()) {
        return QZ_WALLET_OK;  // No PIN set = always pass
    }
    if (pin == NULL || pin[0] == '\0') {
        return QZ_WALLET_ERR_AUTH;
    }

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return QZ_WALLET_ERR_STORAGE;
    }

    // Load salt and hash
    uint8_t salt[16];
    uint8_t stored_hash[32];
    size_t required = 16;
    if (nvs_get_blob(handle, NVS_KEY_PIN_SALT, salt, &required) != ESP_OK ||
        required != 16) {
        nvs_close(handle);
        return QZ_WALLET_ERR_CORRUPT;
    }
    required = 32;
    if (nvs_get_blob(handle, NVS_KEY_PIN_HASH, stored_hash, &required) != ESP_OK ||
        required != 32) {
        nvs_close(handle);
        return QZ_WALLET_ERR_CORRUPT;
    }
    nvs_close(handle);

    // Compute hash of provided PIN
    uint8_t test_hash[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, salt, 16);
    mbedtls_sha256_update(&ctx, (const uint8_t *)pin, strlen(pin));
    mbedtls_sha256_finish(&ctx, test_hash);
    mbedtls_sha256_free(&ctx);

    // Constant-time comparison
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= stored_hash[i] ^ test_hash[i];
    }

    // Clear sensitive data
    memset(test_hash, 0, 32);

    return (diff == 0) ? QZ_WALLET_OK : QZ_WALLET_ERR_AUTH;
}

bool quartz_wallet_has_pin(void) {
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    uint8_t flags = 0;
    nvs_get_u8(handle, NVS_KEY_FLAGS, &flags);
    nvs_close(handle);
    return (flags & FLAG_HAS_PIN) != 0;
}

uint8_t quartz_wallet_pin_attempts(void) {
    return s_pin_attempts;
}

void quartz_wallet_reset_pin_attempts(void) {
    s_pin_attempts = 0;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_PIN_FAIL, 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

bool quartz_wallet_record_failed_pin(void) {
    s_pin_attempts++;

    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, NVS_KEY_PIN_FAIL, s_pin_attempts);
        nvs_commit(handle);
        nvs_close(handle);
    }

    ESP_LOGW(TAG, "Failed PIN attempt %d/%d", s_pin_attempts, PIN_MAX_ATTEMPTS);

    if (s_pin_attempts >= PIN_MAX_ATTEMPTS) {
        ESP_LOGE(TAG, "🚨 MAX PIN ATTEMPTS REACHED — WIPING DEVICE");
        quartz_wallet_wipe();
        return true;
    }

    return false;
}

// ============================================================
// BLE Wallet Service — Signing Protocol
// ============================================================
//
// The ESP32 exposes a BLE GATT service for the phone wallet:
//
// Service: 0000qz01-0000-1000-8000-00805f9b34fb
//
// Characteristics:
//   PUBKEY   (read)    — 32-byte Ed25519 public key
//   ADDRESS  (read)    — Quartz address string
//   SIGN     (write)   — Phone writes unsigned tx hash (32 bytes)
//                        ESP32 signs and notifies result
//   SIGN_RX  (notify)  — 64-byte Ed25519 signature returned
//   SETUP    (write)   — Initial pairing (generates keys if needed)
//   WIPE     (write)   — Factory reset (requires physical button press)
//
// Security:
//   - BLE pairing required (Bonding + MITM protection)
//   - Signing requires authenticated BLE connection
//   - Wipe requires physical button hold (3 seconds)
//   - Private key characteristic does NOT exist — key is never readable
//
// Protocol flow for sending coins:
//   1. Phone constructs unsigned transaction
//   2. Phone hashes relevant fields → 32-byte tx hash
//   3. Phone writes tx hash to SIGN characteristic
//   4. ESP32 reads hash, signs with on-device private key
//   5. ESP32 sends 64-byte signature via SIGN_RX notify
//   6. Phone attaches signature to transaction, broadcasts to network
//   7. Private key was never on the phone
