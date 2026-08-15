/**
 * quartz_wots.h — WOTS+ Quantum-Resistant Signatures for ESP32
 *
 * WHY WOTS+:
 * - Hash-based: only needs SHA-256 (already have mbedtls)
 * - Quantum-resistant: no known quantum attack on hash functions
 * - Small code footprint: ~200 lines of C
 * - Feasible on ESP32: 67 hash chains, each 15 iterations max
 *
 * TRADEOFFS:
 * - One-time signatures: each keypair signs ONE transaction
 * - Merkle tree wraps multiple WOTS+ keys into one address
 * - Signature size: ~2.1KB (67 × 32 bytes)
 *
 * PARAMETER SET (WINTERNITZ w=4):
 * - Hash: SHA-256 (32-byte output)
 * - Chains: 67 (64 for message + 3 for checksum)
 * - Chain length: 2^4 - 1 = 15 iterations
 * - Public key: 67 × 32 = 2,144 bytes
 * - Signature: 67 × 32 = 2,144 bytes
 * - Merkle tree height: 8 (256 one-time keys per address)
 *
 * SECURITY LEVEL: ~128 bits classical, ~64 bits quantum (Grover)
 * For higher security: use SHA-512 or w=2 (larger signatures)
 */

#ifndef QUARTZ_WOTS_H
#define QUARTZ_WOTS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Parameters === */

#define QZ_WOTS_HASH_SIZE   32      /* SHA-256 output */
#define QZ_WOTS_W           4       /* Winternitz parameter */
#define QZ_WOTS_CHAIN_LEN   15      /* 2^w - 1 */
#define QZ_WOTS_MSG_CHAINS  64      /* 256 bits / 4 bits per chain */
#define QZ_WOTS_CKSUM_CHAINS 3      /* checksum chains */
#define QZ_WOTS_TOTAL       67      /* total chains */
#define QZ_WOTS_SIG_SIZE    (QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE)  /* 2144 bytes */
#define QZ_WOTS_PUBKEY_SIZE (QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE)  /* 2144 bytes */

/* Merkle tree: 256 OTS keys per address */
#define QZ_MERKLE_HEIGHT    8
#define QZ_MERKLE_LEAVES    (1 << QZ_MERKLE_HEIGHT)   /* 256 */
#define QZ_MERKLE_AUTH_SIZE (QZ_MERKLE_HEIGHT * QZ_WOTS_HASH_SIZE)  /* 256 bytes */

/* Full signature = WOTS+ sig + Merkle auth path + leaf index */
#define QZ_QSIG_SIZE        (QZ_WOTS_SIG_SIZE + QZ_MERKLE_AUTH_SIZE + 4)  /* ~2404 bytes */

/* Address = Merkle root (32 bytes) — like a Bitcoin address but quantum-safe */
#define QZ_QADDR_SIZE       32

/* Last signature is reserved for key rotation (self-transfer to next address) */
#define QZ_RESERVED_ROTATION_SIG 1

/* === State tracking === */
typedef struct {
    uint8_t  merkle_root[QZ_QADDR_SIZE];   /* quantum address */
    uint16_t next_ots_index;                /* which one-time key to use next */
    uint16_t max_ots_index;                 /* QZ_MERKLE_LEAVES */
    bool     initialized;
    bool     rotation_mode;                 /* true = signing a self-transfer for key rotation */
} qz_qwallet_t;

/* === API === */

/**
 * Generate a new quantum-resistant wallet.
 * Derives 256 WOTS+ keypairs, builds Merkle tree, stores to NVS.
 * The seed determines all keys deterministically.
 *
 * @param seed      32-byte random seed
 * @param wallet    Output wallet state
 * @return 0 on success
 */
int quartz_qwallet_create(const uint8_t seed[32], qz_qwallet_t *wallet);

/**
 * Load wallet from NVS.
 */
int quartz_qwallet_load(qz_qwallet_t *wallet);

/**
 * Sign a message (transaction hash) with the next available OTS key.
 * The last signature (#256) is RESERVED for key rotation self-transfer.
 * Regular signing stops at #255. Use quartz_qwallet_sign_rotation() for the
 * final self-transfer to the next derived address.
 *
 * @param wallet     Wallet state (will advance next_ots_index)
 * @param msg_hash   32-byte message hash to sign
 * @param sig_out    Output buffer of QZ_QSIG_SIZE bytes
 * @param sig_len    Output: actual signature length
 * @return 0 on success, -1 if no OTS keys left (or only reserved sig remains)
 */
int quartz_qwallet_sign(
    const qz_qwallet_t *wallet,
    const uint8_t msg_hash[32],
    uint8_t *sig_out,
    size_t *sig_len
);

/**
 * Sign a key-rotation self-transfer using the reserved last OTS key.
 * This is the ONLY way to use the final signature (#256).
 * After this call, the wallet MUST be rotated to the next address.
 *
 * @param wallet     Wallet state (will advance to max_ots_index)
 * @param msg_hash   32-byte message hash (the self-transfer tx)
 * @param sig_out    Output buffer of QZ_QSIG_SIZE bytes
 * @param sig_len    Output: actual signature length
 * @return 0 on success, -1 if no reserved signature available
 */
int quartz_qwallet_sign_rotation(
    const qz_qwallet_t *wallet,
    const uint8_t msg_hash[32],
    uint8_t *sig_out,
    size_t *sig_len
);

/**
 * Verify a quantum-resistant signature.
 * Can be called by nodes or other devices.
 *
 * @param merkle_root  Signer's address (Merkle root)
 * @param msg_hash     32-byte message hash
 * @param sig          Signature buffer
 * @param sig_len      Signature length
 * @return 0 if valid, -1 if invalid
 */
int quartz_qwallet_verify(
    const uint8_t merkle_root[QZ_QADDR_SIZE],
    const uint8_t msg_hash[32],
    const uint8_t *sig,
    size_t sig_len
);

/**
 * Get wallet address as hex string.
 */
const char *quartz_qwallet_address_hex(const qz_qwallet_t *wallet);

/**
 * Get remaining signatures (excluding the reserved rotation signature).
 * Reports 255 max, not 256 — the last one is reserved.
 */
int quartz_qwallet_remaining(const qz_qwallet_t *wallet);

/**
 * Get remaining signatures including the reserved one.
 * Used internally for rotation logic and diagnostics.
 */
int quartz_qwallet_remaining_total(const qz_qwallet_t *wallet);

/**
 * Check if wallet needs key rotation (approaching WOTS+ limit).
 * - Returns 1 (warn) at signature #240: "Key rotation needed"
 * - Returns 2 (urgent) at signature #254: "Rotation required — last sig reserved"
 * @return 0 = fine, 1 = warn, 2 = urgent
 */
int quartz_qwallet_rotation_status(const qz_qwallet_t *wallet);

/**
 * Legacy bool wrapper for backward compat.
 * Returns true if rotation_status >= 1.
 */
bool quartz_qwallet_needs_rotation(const qz_qwallet_t *wallet);

#ifdef __cplusplus
}
#endif

#endif /* QUARTZ_WOTS_H */
