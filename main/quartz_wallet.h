/**
 * Quartz ESP32 Hardware Wallet — Header
 *
 * The ESP32 is both miner AND hardware wallet.
 * Keys generated on-chip, stored in encrypted flash, never exported.
 */

#ifndef QUARTZ_WALLET_H
#define QUARTZ_WALLET_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Error codes
typedef enum {
    QZ_WALLET_OK             = 0,
    QZ_WALLET_ERR_NOT_FOUND  = -1,  // no wallet in storage
    QZ_WALLET_ERR_STORAGE    = -2,  // NVS read/write failure
    QZ_WALLET_ERR_CORRUPT    = -3,  // data integrity check failed
    QZ_WALLET_ERR_AUTH       = -4,  // BLE connection not authenticated
    QZ_WALLET_ERR_LOCKED     = -5,  // wallet locked, needs unlock
    QZ_WALLET_ERR_INVALID    = -6,  // invalid input
} quartz_wallet_err_t;

/**
 * Generate new Ed25519 keypair using hardware RNG.
 * Keys stored in encrypted NVS. Never exported.
 *
 * @param testnet  Use testnet address prefix
 * @return QZ_WALLET_OK on success
 */
quartz_wallet_err_t quartz_wallet_generate(bool testnet);

/**
 * Load existing wallet from NVS (called on boot).
 *
 * @return QZ_WALLET_OK, or QZ_WALLET_ERR_NOT_FOUND if no wallet exists
 */
quartz_wallet_err_t quartz_wallet_load(void);

/**
 * Sign a message hash with the on-device private key.
 * This is the ONLY operation that uses the private key.
 * The key itself is never returned or transmitted.
 *
 * @param msg       Message/data to sign
 * @param msg_len   Length of message
 * @param signature Output: 64-byte Ed25519 signature
 * @return QZ_WALLET_OK on success
 */
quartz_wallet_err_t quartz_wallet_sign(const uint8_t *msg, size_t msg_len,
                                        uint8_t signature[64]);

/**
 * Get public key (safe to share — 32 bytes).
 * This is transmitted to the phone over BLE.
 */
const uint8_t *quartz_wallet_get_pubkey(void);

/**
 * Get Quartz address string (safe to share).
 */
const char *quartz_wallet_get_address(void);

/**
 * Generate BIP39 mnemonic for one-time backup display.
 * The seed phrase is derived FROM the on-device private key,
 * shown once, then MUST be wiped with quartz_wallet_wipe_seed_phrase().
 *
 * @param words        Output: 12 BIP39 words (each max 11 chars + null)
 * @param max_word_len Max word length including null terminator
 * @return QZ_WALLET_OK on success
 */
quartz_wallet_err_t quartz_wallet_get_seed_phrase_for_backup(
    char words[12][12], size_t max_word_len);

/**
 * Securely zero the seed phrase from RAM after backup.
 * MUST be called after the user confirms they saved their seed.
 */
void quartz_wallet_wipe_seed_phrase(char words[12][12]);

/**
 * Mark seed phrase backup as confirmed.
 * Sets FLAG_BACKED_UP in NVS — device won't nag about seed on boot.
 * Call after user confirms they wrote down the words.
 */
quartz_wallet_err_t quartz_wallet_confirm_backup(void);

/**
 * Check if seed phrase backup has been confirmed.
 * Returns true if FLAG_BACKED_UP is set in NVS.
 */
bool quartz_wallet_is_backup_confirmed(void);

/**
 * Factory reset — wipe all keys from NVS and RAM.
 * Requires physical button hold to trigger via BLE.
 */
quartz_wallet_err_t quartz_wallet_wipe(void);

// ============================================================
// PIN Protection
// ============================================================

/**
 * Set a PIN (4-8 digits). Hashed with salt before storing in NVS.
 * Empty PIN string removes PIN protection.
 *
 * @param pin    Null-terminated PIN string (digits only)
 * @return QZ_WALLET_OK on success
 */
quartz_wallet_err_t quartz_wallet_set_pin(const char *pin);

/**
 * Verify a PIN against stored hash.
 *
 * @param pin    Null-terminated PIN string
 * @return QZ_WALLET_OK if correct, QZ_WALLET_ERR_AUTH if wrong
 */
quartz_wallet_err_t quartz_wallet_check_pin(const char *pin);

/**
 * Check if a PIN is set.
 */
bool quartz_wallet_has_pin(void);

/**
 * Get number of failed PIN attempts since last success.
 */
uint8_t quartz_wallet_pin_attempts(void);

/**
 * Reset failed attempt counter (called on successful PIN entry).
 */
void quartz_wallet_reset_pin_attempts(void);

/**
 * Increment failed attempt counter. If counter reaches 10,
 * triggers factory wipe (calls quartz_wallet_wipe()).
 *
 * @return true if device was wiped (10 attempts reached)
 */
bool quartz_wallet_record_failed_pin(void);

// --- Ed25519 crypto functions (implemented by linked crypto lib) ---

/**
 * Derive Ed25519 keypair from a 32-byte seed.
 * (Implemented by micro-ecc / esp_tinycrypt / mbedtls)
 */
void quartz_ed25519_keypair(const uint8_t seed[32], uint8_t pubkey[32]);

/**
 * Sign a message with Ed25519 private key seed.
 */
void quartz_ed25519_sign(const uint8_t privkey[32],
                          const uint8_t *msg, size_t msg_len,
                          uint8_t signature[64]);

/**
 * Convert 16 bytes of entropy to BIP39 mnemonic words.
 * Uses official 2048-word English wordlist with SHA-256 checksum.
 */
void quartz_entropy_to_mnemonic(const uint8_t entropy[16],
                                 char words[12][12],
                                 size_t max_word_len);

/**
 * Derive Ed25519 keypair from BIP39 mnemonic words.
 * Pipeline: words → PBKDF2 → SLIP-0010 m/44'/789'/0'/0'/0' → Ed25519.
 * Same words produce the same key in any standard wallet.
 */
void quartz_bip39_derive_key(const char words[12][12],
                              uint8_t privkey[32],
                              uint8_t pubkey[32]);

/** Validate 12-word mnemonic (wordlist + BIP-39 checksum). */
bool quartz_bip39_validate_words(const char words[12][12]);

/** Import a wallet from 12 words (canonical derivation). Checksum-validated.
 *  Replaces any existing wallet. PIN (if set) is preserved. */
quartz_wallet_err_t quartz_wallet_restore(const char words[12][12], bool testnet);

/** True if current wallet is a testnet wallet. */
bool quartz_wallet_is_testnet(void);

/* Deprecated: old one-way privkey→mnemonic encoding. */
/* Kept for reference but not used by new wallet generation. */
void quartz_privkey_to_mnemonic(const uint8_t privkey[32],
                                 char words[12][12],
                                 size_t max_word_len);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_WALLET_H
