/**
 * quartz_pool.h — Decentralized Mesh Mining Pools
 *
 * Quartz replaces centralized mining pools (Slush, Foundry, AntPool) with
 * mesh-native pools formed automatically by LoRa neighbors. No operator,
 * no server, no fees, no payout threshold.
 *
 * == How It Works ==
 *
 * 1. DISCOVERY: ESP32 miners broadcast BEACON packets on LoRa (60s interval).
 *    Nearby miners hear beacons and form a local cluster.
 *
 * 2. COORDINATOR ELECTION: Cluster members elect a COORDINATOR using a
 *    deterministic hash election: lowest (SHA-256(pubkey || epoch)) wins.
 *    Coordinator rotates every EPOCH_BLOCKS (16 blocks, ~32 min).
 *
 * 3. WORK DISTRIBUTION: Coordinator assigns work ranges to pool members:
 *    "Miner A search nonces 0-1M, Miner B search 1M-2M", etc.
 *    Reduces duplicate work across the cluster.
 *
 * 4. SHARE SUBMISSION: Each miner sends "shares" (near-misses) to coordinator
 *    via LoRa. A share = header_hash found at POOL_DIFFICULTY (easier than
 *    network difficulty). Proves the miner is working.
 *
 * 5. BLOCK FOUND: When any miner finds a block at NETWORK_DIFFICULTY, they
 *    submit to coordinator. Coordinator broadcasts the winning block with
 *    a MULTI-OUTPUT coinbase that splits the reward proportionally.
 *
 * 6. REWARD SPLIT: Coinbase TX has multiple outputs — one per pool member,
 *    weighted by their share count. Everyone gets paid in the same block.
 *
 * 7. COORDINATOR ROTATION: Every epoch, new election. If coordinator goes
 *    offline, next-lowest hash takes over. No single point of failure.
 *
 * == Anti-Cheat ==
 *
 * - Coordinator can't steal: Block reward splits are deterministic from
 *   share counts. Coordinator broadcasts the split, but every member
 *   verifies it locally before accepting the block.
 * - Miner can't fake shares: Each share includes a valid PoW at pool
 *   difficulty. Forging shares is as hard as mining at that difficulty.
 * - Sybil resistance: One ESP32 = one pubkey = one share stream.
 *   Attacker would need physical ESP32s to inflate shares.
 * - Coordinator non-response: If coordinator stops broadcasting within
 *   TIMEOUT (3 blocks), automatic re-election.
 *
 * == Solo Mining ==
 *
 * Miners can always opt out of pooling. Set pool_mode = SOLO and mine
 * independently. Full reward if you find a block, zero if you don't.
 */

#ifndef QUARTZ_POOL_H
#define QUARTZ_POOL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "quartz.h"

/* ============ Constants ============ */

#define QZ_POOL_MAX_MEMBERS     32      /* Max miners in a local pool */
#define QZ_POOL_EPOCH_BLOCKS    16      /* Coordinator rotation interval */
#define QZ_POOL_TIMEOUT_BLOCKS  3       /* Blocks without coordinator msg → re-elect */
#define QZ_POOL_SHARE_WINDOW    16      /* Blocks counted for reward split */
#define QZ_POOL_BEACON_INTERVAL 60      /* Seconds between beacons */
#define QZ_POOL_DIFFICULTY_DELTA 4      /* Pool difficulty = network - delta (easier) */

/* Pool share — a near-miss at pool difficulty */
typedef struct {
    uint8_t  miner_pubkey[32];      /* Who found this share */
    uint32_t height;                /* Block height being mined */
    uint64_t nonce;                 /* Nonce that produced the share hash */
    uint8_t  header_hash[32];       /* Hash of the header with this nonce */
    uint8_t  share_sig[64];         /* Ed25519 signature over (header_hash || nonce) */
} qz_pool_share_t;

#define QZ_POOL_SHARE_SIZE (32 + 4 + 8 + 32 + 64)  /* 140 bytes — fits in one LoRa packet */

/* Pool member — tracked by coordinator */
typedef struct {
    uint8_t  pubkey[32];            /* Member's device public key */
    uint8_t  chip_id[6];            /* MAC address for identification */
    uint32_t shares;                /* Share count this epoch */
    uint32_t total_shares;          /* Lifetime shares */
    uint32_t last_share_block;      /* Height of last share submitted */
    int8_t   rssi;                  /* LoRa signal strength (dBm) */
    bool     active;                /* Heard from in last 3 blocks? */
} qz_pool_member_t;

/* Pool reward split entry — one per member in the coinbase */
typedef struct {
    uint8_t  pubkey[32];            /* Member's device public key */
    uint64_t amount;                /* Reward in sats */
} qz_pool_payout_t;

/* Pool config — per-device settings */
typedef enum {
    QZ_POOL_MODE_SOLO = 0,          /* Mine independently */
    QZ_POOL_MODE_MEMBER = 1,        /* Join a pool as member */
    QZ_POOL_MODE_COORDINATOR = 2,   /* Act as pool coordinator */
} qz_pool_mode_t;

/* Pool state */
typedef struct {
    qz_pool_mode_t mode;
    uint8_t  coordinator_pubkey[32]; /* Current coordinator */
    uint32_t epoch_start;            /* Height when current epoch began */
    uint32_t member_count;           /* Active members in pool */
    uint32_t my_shares;              /* My shares this epoch */
    uint32_t pool_total_shares;      /* All shares this epoch */
    uint32_t blocks_found;           /* Blocks found by this pool */
    uint64_t rewards_earned;         /* Total rewards earned (sats) */
} qz_pool_state_t;

/* ============ API ============ */

/**
 * Initialize pool subsystem.
 * Starts in SOLO mode by default.
 */
qz_err_t quartz_pool_init(void);

/**
 * Set pool mode.
 * SOLO: Mine independently.
 * MEMBER: Join nearby pool (discovered via beacons).
 * COORDINATOR: Accept members and distribute work.
 */
qz_err_t quartz_pool_set_mode(qz_pool_mode_t mode);

/**
 * Get current pool state.
 */
const qz_pool_state_t *quartz_pool_get_state(void);

/**
 * Broadcast a pool beacon on LoRa.
 * Members send this every 60s so nearby miners can discover them.
 * Coordinators send this every 60s to announce they're accepting members.
 *
 * @param is_coordinator  True if this device is a coordinator
 * @param member_count    Current member count (coordinators only)
 */
qz_err_t quartz_pool_send_beacon(bool is_coordinator, uint8_t member_count);

/**
 * Process incoming beacon from another miner.
 * Updates member list / coordinator candidates.
 *
 * @param pubkey     Sender's public key
 * @param rssi       LoRa signal strength
 * @param is_coord   Is sender a coordinator?
 * @param members    Sender's member count (if coordinator)
 */
qz_err_t quartz_pool_process_beacon(
    const uint8_t *pubkey,
    int8_t rssi,
    bool is_coord,
    uint8_t members
);

/**
 * Submit a share to the coordinator.
 * Called by MEMBERS when they find a near-miss at pool difficulty.
 *
 * @param height      Block height
 * @param nonce       Nonce found
 * @param header_hash Hash of header with this nonce
 * @return QZ_OK if share submitted, QZ_ERR_NOT_COORD if no coordinator
 */
qz_err_t quartz_pool_submit_share(
    uint32_t height,
    uint64_t nonce,
    const uint8_t *header_hash
);

/**
 * Process a share from a member (COORDINATOR only).
 * Validates the share and adds to member's count.
 *
 * @param share  Share submitted by a member
 * @return QZ_OK if accepted
 */
qz_err_t quartz_pool_process_share(const qz_pool_share_t *share);

/**
 * Compute reward split for current epoch (COORDINATOR only).
 * Distributes block reward proportionally by share count.
 *
 * @param total_reward  Total block reward in sats
 * @param payouts       Output array of payouts
 * @param max_payouts   Max entries in payouts array
 * @param payout_count  Actual number of payouts
 * @return QZ_OK on success
 */
qz_err_t quartz_pool_compute_split(
    uint64_t total_reward,
    qz_pool_payout_t *payouts,
    uint8_t max_payouts,
    uint8_t *payout_count
);

/**
 * Check if coordinator election should happen.
 * Called every block. Returns true if this device should become coordinator.
 *
 * Election: lowest SHA-256(pubkey || epoch_number) wins.
 * Deterministic — all members compute the same result.
 *
 * @param block_height  Current block height
 * @param candidates    Array of candidate pubkeys
 * @param count         Number of candidates
 * @return true if THIS device is the new coordinator
 */
bool quartz_pool_check_election(
    uint32_t block_height,
    const uint8_t (*candidates)[32],
    uint8_t count
);

/**
 * Check if coordinator has timed out.
 * Called every block. If coordinator hasn't been heard from in
 * QZ_POOL_TIMEOUT_BLOCKS, trigger re-election.
 *
 * @param block_height  Current block height
 * @return true if coordinator timed out
 */
bool quartz_pool_coordinator_timeout(uint32_t block_height);

/**
 * Get pool statistics for display.
 */
typedef struct {
    qz_pool_mode_t mode;
    uint32_t member_count;
    uint32_t my_shares;
    uint32_t pool_shares;
    uint32_t blocks_found;
    uint64_t rewards_earned_qz;     /* In QZ (not sats) */
    uint8_t  coordinator_pubkey[6]; /* First 6 bytes for display */
    uint32_t epoch_block;           /* Current epoch progress */
} qz_pool_stats_t;

void quartz_pool_get_stats(qz_pool_stats_t *stats);

#endif /* QUARTZ_POOL_H */
