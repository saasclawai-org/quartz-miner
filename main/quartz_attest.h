/**
 * quartz_attest.h — ESP32 Remote Attestation Protocol
 *
 * Ensures only real ESP32 hardware can mine Quartz blocks. Prevents
 * PC/emulator mining even though CrystalHash PoW can be computed faster
 * on a desktop CPU.
 *
 * == Protocol Overview ==
 *
 * 1. FACTORY KEY PROVISIONING (done once, at first boot or by manufacturer)
 *    - ESP32-S3 generates Ed25519 keypair using hardware RNG
 *    - Private key stored in encrypted NVS (flash encryption + NVS encryption)
 *    - Public key hash burned into eFuse BLOCK_KEY0 (irreversible)
 *    - Secure Boot v2 ensures only signed firmware can access the key
 *
 * 2. DEVICE REGISTRATION (once per device, via WiFi to a seed node)
 *    - ESP32 sends: public_key + attestation_proof
 *    - attestation_proof = HMAC-SHA256(eFuse_key, public_key)
 *      (eFuse key is burned at factory, unique per chip, unreadable by software
 *       — only HMAC engine can use it)
 *    - Seed node verifies: re-derives expected HMAC using the chip's eFuse
 *      root key hash (stored in a public registry), checks match
 *    - Device added to Miner Registry with its public key
 *
 * 3. BLOCK CO-SIGNING (every mined block)
 *    - After finding PoW nonce, ESP32 signs:
 *      signature = Ed25519_sign(private_key, block_header || nonce)
 *    - Block includes: miner_sig (64 bytes) + miner_pubkey (32 bytes)
 *    - Verifying nodes check:
 *      a) Valid PoW (CrystalHash meets difficulty target)
 *      b) Valid Ed25519 signature over (header || nonce)
 *      c) miner_pubkey is in the Miner Registry
 *      d) Device not banned
 *      e) Device hasn't mined on a conflicting fork recently
 *
 * 4. SLASHING (anti-cheat)
 *    - If a device signs two blocks at the same height (fork attempt):
 *      → Evidence submitted to network (both signatures)
 *      → Device banned, remaining balance slashed
 *    - If a device produces blocks faster than physically possible
 *      (> 1 block per 60s on ESP32 hardware): flagged for review
 *
 * == eFuse Usage on ESP32-S3 ==
 *
 * The ESP32-S3 has 11 eFuse blocks (256 bits each):
 *   BLOCK0    — System config (read-only)
 *   BLOCK1-3  — Secure Boot keys (SHA-256 of public key)
 *   BLOCK4-5  — Flash encryption key
 *   BLOCK6    — Custom (available for application use)
 *   BLOCK7-10 — Additional custom blocks
 *
 * Quartz uses:
 *   BLOCK_KEY0 (BLOCK6) — Device attestation key (256-bit, burn-once)
 *   SECURE_BOOT_V2       — Ensures firmware integrity (prerequisite)
 *
 * Once BLOCK6 is burned, it CANNOT be read back by any software —
 * only the hardware HMAC peripheral can use it for HMAC-SHA256 operations.
 * This is the hardware root of trust.
 */

#ifndef QUARTZ_ATTEST_H
#define QUARTZ_ATTEST_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "quartz.h"

/* ============ Constants ============ */

#define QZ_DEVICE_PUBKEY_SIZE   32      /* Ed25519 public key */
#define QZ_DEVICE_SIG_SIZE      64      /* Ed25519 signature */
#define QZ_EFUSE_BLOCK          6       /* eFuse BLOCK6 for attestation key */
#define QZ_ATTEST_HMAC_SIZE     32      /* HMAC-SHA256 output */
#define QZ_DEVICE_ID_SIZE       32      /* Short device identifier (first 16 bytes of pubkey hash) */

/* Registration proof = HMAC(eFuse_key, pubkey) — 32 bytes */
typedef struct {
    uint8_t pubkey[QZ_DEVICE_PUBKEY_SIZE];          /* Device Ed25519 public key */
    uint8_t attestation[QZ_ATTEST_HMAC_SIZE];        /* HMAC-SHA256(eFuse_key, pubkey) */
    uint8_t chip_id[6];                              /* ESP32 MAC address (for identification) */
    uint8_t firmware_hash[32];                       /* SHA-256 of running firmware (for version tracking) */
} qz_registration_t;

#define QZ_REGISTRATION_SIZE (32 + 32 + 6 + 32)     /* 102 bytes */

/* Block attestation = Ed25519 signature over (header || nonce) */
typedef struct {
    uint8_t pubkey[QZ_DEVICE_PUBKEY_SIZE];           /* Which device mined this block */
    uint8_t signature[QZ_DEVICE_SIG_SIZE];           /* Ed25519(header_hash || nonce) */
} qz_block_attestation_t;

#define QZ_ATTESTATION_SIZE (32 + 64)                /* 96 bytes added to each block */

/* Device states in the registry */
typedef enum {
    QZ_DEVICE_UNKNOWN = 0,      /* Not registered */
    QZ_DEVICE_ACTIVE = 1,       /* Registered, in good standing */
    QZ_DEVICE_FLAGGED = 2,      /* Suspicious activity (too-fast mining) */
    QZ_DEVICE_BANNED = 3,       /* Slashed (double-sign or proven emulation) */
} qz_device_status_t;

/* Slashing evidence — submitted when a device misbehaves */
typedef struct {
    uint8_t pubkey[QZ_DEVICE_PUBKEY_SIZE];
    uint8_t block_hash_1[32];   /* First conflicting block */
    uint8_t block_hash_2[32];   /* Second conflicting block at same height */
    uint8_t sig_1[QZ_DEVICE_SIG_SIZE];
    uint8_t sig_2[QZ_DEVICE_SIG_SIZE];
    uint32_t height;            /* Height where conflict occurred */
} qz_slash_evidence_t;

/* ============ API (Firmware Side) ============ */

/**
 * Initialize device attestation.
 * - Loads or generates device keypair
 * - Verifies eFuse attestation key is burned
 * - Loads private key from encrypted NVS
 *
 * @return QZ_OK if device is provisioned, QZ_ERR_NOT_FOUND if needs registration
 */
qz_err_t quartz_attest_init(void);

/**
 * Check if device has been provisioned (keys generated + eFuse burned).
 */
bool quartz_attest_is_provisioned(void);

/**
 * Get this device's public key.
 * Safe to share — public key is not secret.
 */
const uint8_t *quartz_attest_get_pubkey(void);

/**
 * Get short device ID (first 16 bytes of SHA-256(pubkey)).
 * Used for display and quick identification.
 */
void quartz_attest_get_device_id(uint8_t out[QZ_DEVICE_ID_SIZE]);

/**
 * Create registration packet for first-time device registration.
 * Includes pubkey + HMAC attestation proof + chip ID + firmware hash.
 *
 * @param out  Output buffer (at least QZ_REGISTRATION_SIZE bytes)
 * @return QZ_OK on success
 */
qz_err_t quartz_attest_create_registration(qz_registration_t *out);

/**
 * Sign a block after finding a valid PoW nonce.
 *
 * Signs SHA-256(block_header_serialized) with the device's Ed25519 private key.
 * The private key never leaves NVS — signing happens in a secure context.
 *
 * @param header_bytes  80-byte serialized block header (with nonce filled in)
 * @param header_hash   32-byte SHA-256 of header_bytes
 * @param nonce         The winning nonce value
 * @param out           Output attestation (pubkey + signature)
 * @return QZ_OK on success
 */
qz_err_t quartz_attest_sign_block(
    const uint8_t *header_bytes,
    size_t header_len,
    const uint8_t *header_hash,
    uint64_t nonce,
    qz_block_attestation_t *out
);

/**
 * Provision device (first-time setup).
 * - Generates Ed25519 keypair using hardware RNG
 * - Stores private key in encrypted NVS
 * - Burns public key hash into eFuse BLOCK6
 * - Computes HMAC attestation proof
 *
 * WARNING: This is a one-way operation. Once eFuse is burned, it cannot be undone.
 * Called automatically on first boot if not already provisioned.
 *
 * @return QZ_OK on success, QZ_ERR_ALREADY if already provisioned
 */
qz_err_t quartz_attest_provision(void);

/* ============ API (Verifier Side — used by nodes) ============ */

/**
 * Verify a block's attestation signature.
 *
 * Checks:
 * 1. Ed25519 signature is valid over header_hash
 * 2. (Caller must check pubkey is in registry + not banned)
 *
 * @param header_hash   32-byte block header hash
 * @param nonce         Block nonce (included in signed message)
 * @param attestation   Block attestation (pubkey + signature)
 * @return QZ_OK if signature valid
 */
qz_err_t quartz_attest_verify_block(
    const uint8_t *header_hash,
    uint64_t nonce,
    const qz_block_attestation_t *attestation
);

/**
 * Verify a registration proof.
 *
 * In a full implementation, this would verify the HMAC against the
 * ESP32-S3 eFuse root of trust. For the reference node, we simulate
 * with a known-good registry of manufacturer-provisioned devices.
 *
 * @param reg  Registration packet from device
 * @return QZ_OK if attestation is valid
 */
qz_err_t quartz_attest_verify_registration(const qz_registration_t *reg);

/**
 * Verify slashing evidence.
 *
 * Checks that both signatures are valid for the same pubkey,
 * over different block hashes at the same height.
 *
 * @param evidence  Slashing evidence
 * @return QZ_OK if evidence is valid (device should be banned)
 */
qz_err_t quartz_attest_verify_slash(const qz_slash_evidence_t *evidence);

#endif /* QUARTZ_ATTEST_H */
