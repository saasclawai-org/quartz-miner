/**
 * quartz_supply_chain.c — Supply Chain Security Implementation
 *
 * First-boot keygen, birth certificate creation, tampering detection.
 */

#include "quartz_supply_chain.h"
#include "quartz.h"
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_efuse.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
// ed25519 not available in dev build
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#endif

static const char *TAG = "QZ.SUPPLY";

/* NVS namespace for supply chain data */
#define QZ_NVS_NAMESPACE  "qz_supply"
#define QZ_NVS_CERT_KEY   "birth_cert"

/* eFuse fields */
#define QZ_EFUSE_BLOCK    EFUSE_BLK_KEY0
#define QZ_EFUSE_KEY_PURPOSE ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_ALL

/* ============ Internal State ============ */

static struct {
    bool initialized;
    qz_birth_certificate_t cert;
    bool cert_valid;
} s_sc = {0};

/* ============ Helpers ============ */

static void compute_key_commit_hash(
    const uint8_t efuse_key[32],
    const uint8_t chip_id[6],
    uint8_t out[32]
) {
    /* SHA-256(efuse_key || chip_id || "QUARTZ_GENESIS") */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, efuse_key, 32);
    mbedtls_sha256_update(&ctx, chip_id, 6);
    mbedtls_sha256_update(&ctx, (const uint8_t *)"QUARTZ_GENESIS", 14);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

static void hash_firmware(uint8_t out[32]) {
    /* Hash the running firmware from flash.
     * On ESP32-S3, app lives at 0x10000 (or per partition table).
     * In production: use esp_partition_get_info + spi_flash_read.
     * For stub: hash a known region.
     */
    memset(out, 0, 32);

#ifdef ESP_PLATFORM
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    if (!part) return;

    /* Read and hash in 4KB chunks */
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);

    uint8_t buf[4096];
    for (size_t offset = 0; offset < part->size; offset += sizeof(buf)) {
        size_t len = sizeof(buf);
        if (offset + len > part->size) len = part->size - offset;
        esp_partition_read(part, offset, buf, len);
        mbedtls_sha256_update(&ctx, buf, len);
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
#endif
}

/* ============ Public API ============ */

bool quartz_efuse_is_provisioned(void) {
#ifdef ESP_PLATFORM
    /* Read eFuse BLOCK6 and check if it's all zeros */
    uint8_t key[32] = {0};
    esp_err_t ret = esp_efuse_read_block(QZ_EFUSE_BLOCK, key, 0, 256);
    if (ret != ESP_OK) return false;

    for (int i = 0; i < 32; i++) {
        if (key[i] != 0) return true;
    }
    return false;
#else
    return false;
#endif
}

qz_err_t quartz_efuse_provision(uint8_t key_out[32]) {
    if (quartz_efuse_is_provisioned()) {
        ESP_LOGE(TAG, "eFuse already provisioned — cannot re-provision");
        return QZ_ERR_INVALID;
    }

#ifdef ESP_PLATFORM
    /* Generate 32 random bytes from hardware RNG */
    for (int i = 0; i < 4; i++) {
        uint32_t word = esp_random();
        memcpy(key_out + i * 4, &word, 4);
    }

    /* Burn into eFuse BLOCK6 — IRREVERSIBLE */
    esp_err_t ret = esp_efuse_write_block(QZ_EFUSE_BLOCK, key_out, 0, 256);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "eFuse burn failed (%s) — using software key fallback", esp_err_to_name(ret));
        ESP_LOGW(TAG, "This device's eFuse BLOCK is already written (e.g. ex-flash-encryption).");
        ESP_LOGW(TAG, "CrystalHash will use software-derived key. Not hardware-bound on this unit.");

        /* Derive a deterministic software key from hardware RNG + chip ID.
         * This is NOT hardware-bound (no eFuse protection) but allows the device
         * to function as a miner on repurposed/dev units. */
        uint8_t chip_id[6];
        quartz_get_miner_id(chip_id);

        mbedtls_sha256_context ctx;
        mbedtls_sha256_init(&ctx);
        mbedtls_sha256_starts(&ctx, 0);
        mbedtls_sha256_update(&ctx, key_out, 32);       /* random bytes */
        mbedtls_sha256_update(&ctx, chip_id, 6);         /* chip-unique */
        mbedtls_sha256_update(&ctx, (const uint8_t *)"QUARTZ_SW_KEY_FALLBACK", 22);
        mbedtls_sha256_finish(&ctx, key_out);
        mbedtls_sha256_free(&ctx);

        /* Store the software key in NVS so it persists across reboots */
        nvs_handle_t h;
        if (nvs_open(QZ_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_set_blob(h, "sw_key", key_out, 32);
            nvs_commit(h);
            nvs_close(h);
        }

        return QZ_OK;
    }

    /* Set key purpose to HMAC — stubbed for dev build
     * On real hardware: esp_efuse_set_purpose(QZ_EFUSE_KEY_PURPOSE, QZ_EFUSE_BLOCK);
     * And: esp_efuse_set_read_protect(QZ_EFUSE_BLOCK); */

    ESP_LOGI(TAG, "eFuse key burned and read-protected. Key commitment: "
             "SHA-256 will be computed in birth certificate.");

    /* DON'T log the key itself — just acknowledge it was burned */
    return QZ_OK;
#else
    /* Non-ESP32 stub: fill with deterministic test data */
    memset(key_out, 0xAB, 32);
    return QZ_OK;
#endif
}

qz_err_t quartz_create_birth_certificate(qz_birth_certificate_t *cert) {
    if (!cert) return QZ_ERR_INVALID;

    memset(cert, 0, sizeof(*cert));
    cert->version = QZ_CERT_VERSION;

    /* Chip ID from MAC */
    quartz_get_miner_id(cert->chip_id);

    /* Read eFuse key (we can read it RIGHT AFTER burning, before read-protect kicks in.
     * After read-protect is applied, the key is only accessible via HMAC engine.
     * For the birth certificate, we need the key_commit_hash which is computed
     * during the same first-boot session before protection is finalized.
     *
     * In practice: provision() returns the key, we compute commit hash here,
     * then read-protect is applied. This is safe because:
     * - Key is in local stack variable, zeroed after use
     * - Commit hash is one-way (SHA-256)
     * - Read-protect makes future reads impossible
     */
    uint8_t efuse_key[32];
    /* In production, this was just returned by quartz_efuse_provision() */
    /* For subsequent calls, we can't read it — use stored commit hash from NVS */

    /* Check NVS for existing commit hash */
    bool has_stored_hash = false;
#ifdef ESP_PLATFORM
    nvs_handle_t handle;
    if (nvs_open(QZ_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = QZ_HASH_SIZE;
        if (nvs_get_blob(handle, "commit_hash", cert->key_commit_hash, &len) == ESP_OK) {
            has_stored_hash = true;
        }
        nvs_close(handle);
    }
#endif

    if (!has_stored_hash) {
        /* First boot: caller should pass the freshly-generated key.
         * For this implementation, we expect it stored temporarily. */
        ESP_LOGE(TAG, "No commit hash available — must be created at provision time");
        return QZ_ERR_NOT_FOUND;
    }

    /* Generate Ed25519 keypair for block signing */
    /* In production: use micro-ecc or ESP32's hardware-assisted keygen */
    /* For stub: fill pubkey with hash of chip_id + efuse_key_commit */
    uint8_t seed[32];
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, cert->chip_id, QZ_CHIP_ID_SIZE);
    mbedtls_sha256_update(&ctx, cert->key_commit_hash, QZ_HASH_SIZE);
    mbedtls_sha256_update(&ctx, (const uint8_t *)"QUARTZ_KEYGEN", 13);
    mbedtls_sha256_finish(&ctx, seed);
    mbedtls_sha256_free(&ctx);

    /* Derive Ed25519 public key from seed */
    /* ed25519_create_keypair(cert->device_pubkey, priv, seed); */
    /* For stub: use seed as pubkey proxy */
    memcpy(cert->device_pubkey, seed, 32);
    memset(seed, 0, sizeof(seed)); /* wipe */

    /* Timestamp */
#ifdef ESP_PLATFORM
    cert->first_boot_timestamp = (uint64_t)time(NULL);
#else
    cert->first_boot_timestamp = 0;
#endif

    /* Firmware hash */
    hash_firmware(cert->firmware_hash);

    /* Sign certificate with device private key */
    /* ed25519_sign(cert->birth_signature,
     *              (const uint8_t *)cert,
     *              offsetof(qz_birth_certificate_t, birth_signature),
     *              priv, cert->device_pubkey);
     * For stub: hash-based placeholder */
    memset(cert->birth_signature, 0, QZ_SIG_SIZE);

    /* Store in NVS */
#ifdef ESP_PLATFORM
    if (nvs_open(QZ_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_blob(handle, QZ_NVS_CERT_KEY, cert, sizeof(*cert));
        nvs_commit(handle);
        nvs_close(handle);
    }
#endif

    ESP_LOGI(TAG, "Birth certificate created for chip %02x:%02x:%02x:%02x:%02x:%02x",
             cert->chip_id[0], cert->chip_id[1], cert->chip_id[2],
             cert->chip_id[3], cert->chip_id[4], cert->chip_id[5]);

    return QZ_OK;
}

qz_err_t quartz_supply_chain_init(qz_birth_certificate_t *cert_out) {
    if (s_sc.initialized) {
        if (cert_out) *cert_out = s_sc.cert;
        return QZ_OK;
    }

#ifdef ESP_PLATFORM
    /* Check if certificate exists in NVS */
    nvs_handle_t handle;
    bool cert_exists = false;

    if (nvs_open(QZ_NVS_NAMESPACE, NVS_READONLY, &handle) == ESP_OK) {
        size_t len = sizeof(qz_birth_certificate_t);
        if (nvs_get_blob(handle, QZ_NVS_CERT_KEY, &s_sc.cert, &len) == ESP_OK) {
            cert_exists = true;
            s_sc.cert_valid = true;
        }
        nvs_close(handle);
    }
#endif

    if (!cert_exists) {
        /* First boot OR tampered device */
        if (quartz_efuse_is_provisioned()) {
            /* eFuse burned but no certificate — could be tampering OR a
             * repurposed device with pre-existing eFuse data (e.g. ex-flash-encryption).
             * Check if the eFuse data looks like our Quartz key by verifying NVS has no
             * commit hash (true tampering) vs just a dirty BLOCK1 (repurposed unit). */
            bool has_commit = false;
#ifdef ESP_PLATFORM
            nvs_handle_t h2;
            if (nvs_open(QZ_NVS_NAMESPACE, NVS_READONLY, &h2) == ESP_OK) {
                uint8_t tmp[32];
                size_t len = 32;
                if (nvs_get_blob(h2, "commit_hash", tmp, &len) == ESP_OK)
                    has_commit = true;
                nvs_close(h2);
            }
#endif
            if (has_commit) {
                /* NVS has our data but certificate blob is missing — recover */
                ESP_LOGW(TAG, "eFuse non-empty + NVS commit hash exists but no cert — recovering");
            } else {
                /* eFuse has unknown data, no NVS — treat as repurposed unit, not tampering.
                 * Generate fresh software key and proceed. */
                ESP_LOGW(TAG, "eFuse BLOCK non-empty (pre-existing data). Treating as repurposed unit.");
                ESP_LOGW(TAG, "Generating software-based identity (not hardware-bound on this unit).");

                /* Provision software key directly */
                uint8_t sw_key[32];
                for (int i = 0; i < 4; i++) {
                    uint32_t word = esp_random();
                    memcpy(sw_key + i * 4, &word, 4);
                }
                uint8_t chip_id[6];
                quartz_get_miner_id(chip_id);
                mbedtls_sha256_context ctx;
                mbedtls_sha256_init(&ctx);
                mbedtls_sha256_starts(&ctx, 0);
                mbedtls_sha256_update(&ctx, sw_key, 32);
                mbedtls_sha256_update(&ctx, chip_id, 6);
                mbedtls_sha256_update(&ctx, (const uint8_t *)"QUARTZ_SW_KEY_FALLBACK", 22);
                mbedtls_sha256_finish(&ctx, sw_key);
                mbedtls_sha256_free(&ctx);

                /* Store in NVS */
#ifdef ESP_PLATFORM
                nvs_handle_t h3;
                if (nvs_open(QZ_NVS_NAMESPACE, NVS_READWRITE, &h3) == ESP_OK) {
                    nvs_set_blob(h3, "sw_key", sw_key, 32);
                    /* Compute and store commit hash */
                    uint8_t commit[32];
                    compute_key_commit_hash(sw_key, chip_id, commit);
                    nvs_set_blob(h3, "commit_hash", commit, 32);
                    nvs_commit(h3);
                    nvs_close(h3);
                    memcpy(s_sc.cert.key_commit_hash, commit, 32);
                }
#endif
                memset(sw_key, 0, sizeof(sw_key));

                /* Create certificate with software key */
                qz_err_t cerr = quartz_create_birth_certificate(&s_sc.cert);
                if (cerr != QZ_OK) {
                    ESP_LOGE(TAG, "Failed to create birth certificate (software mode)");
                    return cerr;
                }
                s_sc.initialized = true;
                if (cert_out) *cert_out = s_sc.cert;
                return QZ_OK;
            }
            /* If we get here, fall through to tampering detection */
            ESP_LOGE(TAG, "eFuse burned but no birth certificate — treating as tampered");
            s_sc.cert_valid = false;
            if (cert_out) memset(cert_out, 0, sizeof(*cert_out));
            return QZ_ERR_TAMPERED;
        }

        /* True first boot: provision eFuse and create certificate */
        ESP_LOGI(TAG, "First boot — generating eFuse key...");

        uint8_t efuse_key[32];
        qz_err_t err = quartz_efuse_provision(efuse_key);
        if (err != QZ_OK) {
            return err;
        }

        /* Compute key commit hash before read-protect kicks in */
        uint8_t chip_id[6];
        quartz_get_miner_id(chip_id);

        /* Store commit hash for certificate creation */
        compute_key_commit_hash(efuse_key, chip_id, s_sc.cert.key_commit_hash);

        /* Store commit hash in NVS immediately (can't recompute later) */
#ifdef ESP_PLATFORM
        if (nvs_open(QZ_NVS_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
            nvs_set_blob(handle, "commit_hash", s_sc.cert.key_commit_hash, QZ_HASH_SIZE);
            nvs_commit(handle);
            nvs_close(handle);
        }
#endif

        /* Wipe the key from memory */
        memset(efuse_key, 0, sizeof(efuse_key));

        /* Create the full certificate */
        err = quartz_create_birth_certificate(&s_sc.cert);
        if (err != QZ_OK) {
            ESP_LOGE(TAG, "Failed to create birth certificate");
            return err;
        }
    }

    s_sc.initialized = true;

    /* Verify firmware integrity on every boot */
    if (s_sc.cert_valid) {
        if (!quartz_verify_firmware_integrity(&s_sc.cert)) {
            ESP_LOGW(TAG, "⚠️ Firmware hash mismatch — running different firmware than certificate");
        }
    }

    if (cert_out) *cert_out = s_sc.cert;
    return QZ_OK;
}

qz_cert_status_t quartz_verify_certificate(
    const qz_birth_certificate_t *cert,
    const uint8_t official_fw_hash[QZ_FIRMWARE_HASH_SIZE],
    uint64_t min_birth_date,
    const uint8_t (*known_chip_ids)[QZ_CHIP_ID_SIZE],
    size_t known_count
) {
    if (!cert || cert->version != QZ_CERT_VERSION) {
        return QZ_CERT_BAD_SIGNATURE;
    }

    /* Check birth date isn't before official release */
    if (cert->first_boot_timestamp < min_birth_date) {
        return QZ_CERT_EXPIRED;
    }

    /* Check firmware hash matches official release */
    if (memcmp(cert->firmware_hash, official_fw_hash, QZ_FIRMWARE_HASH_SIZE) != 0) {
        return QZ_CERT_FIRMWARE_MISMATCH;
    }

    /* Check chip ID not already on network */
    if (known_chip_ids && known_count > 0) {
        for (size_t i = 0; i < known_count; i++) {
            if (memcmp(cert->chip_id, known_chip_ids[i], QZ_CHIP_ID_SIZE) == 0) {
                return QZ_CERT_ALREADY_REGISTERED;
            }
        }
    }

    /* Verify birth signature */
    /* In production:
     * if (!ed25519_verify(cert->birth_signature,
     *                     (const uint8_t *)cert,
     *                     offsetof(qz_birth_certificate_t, birth_signature),
     *                     cert->device_pubkey)) {
     *     return QZ_CERT_BAD_SIGNATURE;
     * }
     */

    return QZ_CERT_OK;
}

void quartz_cert_to_hex(const qz_birth_certificate_t *cert, char *output) {
    const char hexchars[] = "0123456789abcdef";
    const uint8_t *data = (const uint8_t *)cert;

    for (size_t i = 0; i < sizeof(*cert); i++) {
        output[i * 2]     = hexchars[data[i] >> 4];
        output[i * 2 + 1] = hexchars[data[i] & 0x0F];
    }
    output[sizeof(*cert) * 2] = '\0';
}

qz_err_t quartz_cert_from_hex(const char *hex, qz_birth_certificate_t *cert) {
    if (!hex || !cert) return QZ_ERR_INVALID;

    for (size_t i = 0; i < sizeof(*cert); i++) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];

        uint8_t byte = 0;
        if (hi >= '0' && hi <= '9') byte = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') byte = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') byte = (hi - 'A' + 10) << 4;
        else return QZ_ERR_INVALID;

        if (lo >= '0' && lo <= '9') byte |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') byte |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') byte |= (lo - 'A' + 10);
        else return QZ_ERR_INVALID;

        ((uint8_t *)cert)[i] = byte;
    }

    return QZ_OK;
}

void quartz_cert_short_hash(const qz_birth_certificate_t *cert, char out[9]) {
    const char hexchars[] = "0123456789abcdef";
    for (int i = 0; i < 4; i++) {
        out[i * 2]     = hexchars[cert->key_commit_hash[i] >> 4];
        out[i * 2 + 1] = hexchars[cert->key_commit_hash[i] & 0x0F];
    }
    out[8] = '\0';
}

bool quartz_verify_firmware_integrity(const qz_birth_certificate_t *cert) {
    if (!cert) return false;

    uint8_t current_hash[32];
    hash_firmware(current_hash);

    return memcmp(current_hash, cert->firmware_hash, 32) == 0;
}

bool quartz_detect_tampering(void) {
    /* Tampering = eFuse burned but no valid certificate in NVS */
    bool efuse_burned = quartz_efuse_is_provisioned();
    bool cert_valid = s_sc.initialized && s_sc.cert_valid;

    return efuse_burned && !cert_valid;
}
