/**
 * quartz_pool.c — Mesh Mining Pool Implementation
 *
 * Coordinator and member logic for decentralized Quartz mining pools.
 * Runs over LoRa mesh — no server, no internet needed.
 */

#include "quartz_pool.h"
#include "quartz.h"
#include "quartz_attest.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#endif

static const char *TAG = "QZ.POOL";

/* ============ Internal State ============ */

static struct {
    bool initialized;
    qz_pool_mode_t mode;

    /* Coordinator state */
    qz_pool_member_t members[QZ_POOL_MAX_MEMBERS];
    uint8_t member_count;

    /* Member state */
    uint8_t coordinator_pubkey[32];
    uint32_t last_coord_block;
    bool coordinator_known;

    /* Shared state */
    qz_pool_state_t state;
    uint32_t current_epoch;
} s_pool = {0};

/* ============ Helpers ============ */

static uint32_t get_epoch(uint32_t block_height) {
    return block_height / QZ_POOL_EPOCH_BLOCKS;
}

static void hash_for_election(const uint8_t pubkey[32], uint32_t epoch, uint8_t out[32]) {
    /* SHA-256(pubkey || epoch_4bytes_LE) */
    /* In production: use mbedTLS or hardware SHA-256 */
    uint8_t buf[36];
    memcpy(buf, pubkey, 32);
    memcpy(buf + 32, &epoch, 4);

    /* Simplified hash for stub — production uses mbedtls_sha256 */
    memset(out, 0, 32);
    for (int i = 0; i < 36; i++) {
        out[i % 32] ^= buf[i];
        out[(i + 7) % 32] += buf[i] * 31;
    }
}

static qz_pool_member_t *find_member(const uint8_t pubkey[32]) {
    for (int i = 0; i < s_pool.member_count; i++) {
        if (memcmp(s_pool.members[i].pubkey, pubkey, 32) == 0) {
            return &s_pool.members[i];
        }
    }
    return NULL;
}

static qz_pool_member_t *add_member(const uint8_t pubkey[32], const uint8_t chip_id[6]) {
    if (s_pool.member_count >= QZ_POOL_MAX_MEMBERS) {
        ESP_LOGW(TAG, "Pool full (%d members)", QZ_POOL_MAX_MEMBERS);
        return NULL;
    }

    /* Check for duplicate */
    if (find_member(pubkey)) {
        return NULL;
    }

    qz_pool_member_t *m = &s_pool.members[s_pool.member_count++];
    memcpy(m->pubkey, pubkey, 32);
    if (chip_id) memcpy(m->chip_id, chip_id, 6);
    m->shares = 0;
    m->total_shares = 0;
    m->last_share_block = 0;
    m->rssi = 0;
    m->active = true;

    ESP_LOGI(TAG, "Added pool member %d/%d", s_pool.member_count, QZ_POOL_MAX_MEMBERS);
    return m;
}

/* ============ Public API ============ */

qz_err_t quartz_pool_init(void) {
    if (s_pool.initialized) return QZ_OK;
    memset(&s_pool, 0, sizeof(s_pool));
    s_pool.mode = QZ_POOL_MODE_SOLO;
    s_pool.initialized = true;
    ESP_LOGI(TAG, "Pool initialized (SOLO mode)");
    return QZ_OK;
}

qz_err_t quartz_pool_set_mode(qz_pool_mode_t mode) {
    if (!s_pool.initialized) return QZ_ERR_NOT_FOUND;

    if (mode == s_pool.mode) return QZ_OK;

    /* Clear state on mode change */
    if (mode == QZ_POOL_MODE_SOLO) {
        memset(s_pool.members, 0, sizeof(s_pool.members));
        s_pool.member_count = 0;
        s_pool.coordinator_known = false;
    }

    if (mode == QZ_POOL_MODE_COORDINATOR) {
        /* Add ourselves as first member */
        const uint8_t *mypub = quartz_attest_get_pubkey();
        if (mypub) {
            add_member(mypub, NULL);
        }
    }

    s_pool.mode = mode;
    s_pool.state.mode = mode;

    const char *mode_str = "SOLO";
    if (mode == QZ_POOL_MODE_MEMBER) mode_str = "MEMBER";
    if (mode == QZ_POOL_MODE_COORDINATOR) mode_str = "COORDINATOR";
    ESP_LOGI(TAG, "Pool mode → %s", mode_str);

    return QZ_OK;
}

const qz_pool_state_t *quartz_pool_get_state(void) {
    return &s_pool.state;
}

qz_err_t quartz_pool_send_beacon(bool is_coordinator, uint8_t member_count) {
    /* In production: build LoRa BEACON packet with:
     *   - pubkey (32 bytes)
     *   - flags (1 byte: is_coordinator | member_count)
     *   - chip_id (6 bytes)
     *   Total: 39 bytes — well within LoRa payload
     */
    (void)is_coordinator;
    (void)member_count;
    return QZ_OK;
}

qz_err_t quartz_pool_process_beacon(
    const uint8_t *pubkey,
    int8_t rssi,
    bool is_coord,
    uint8_t members
) {
    if (!s_pool.initialized) return QZ_ERR_NOT_FOUND;

    /* If we're a coordinator, track as potential member */
    if (s_pool.mode == QZ_POOL_MODE_COORDINATOR) {
        qz_pool_member_t *m = find_member(pubkey);
        if (!m) {
            m = add_member(pubkey, NULL);
        }
        if (m) {
            m->rssi = rssi;
            m->active = true;
        }
    }

    /* If we're a member or solo, track coordinator candidates */
    if (s_pool.mode != QZ_POOL_MODE_COORDINATOR && is_coord) {
        memcpy(s_pool.coordinator_pubkey, pubkey, 32);
        s_pool.coordinator_known = true;
        ESP_LOGI(TAG, "Heard coordinator (members=%d, rssi=%d)", members, rssi);
    }

    return QZ_OK;
}

qz_err_t quartz_pool_submit_share(
    uint32_t height,
    uint64_t nonce,
    const uint8_t *header_hash
) {
    if (!s_pool.initialized) return QZ_ERR_NOT_FOUND;
    if (s_pool.mode != QZ_POOL_MODE_MEMBER) return QZ_ERR_INVALID;
    if (!s_pool.coordinator_known) return QZ_ERR_NOT_FOUND;

    /* Build share packet */
    qz_pool_share_t share;
    const uint8_t *mypub = quartz_attest_get_pubkey();
    if (!mypub) return QZ_ERR_NOT_FOUND;

    memcpy(share.miner_pubkey, mypub, 32);
    share.height = height;
    share.nonce = nonce;
    memcpy(share.header_hash, header_hash, 32);

    /* Sign the share */
    /* In production: quartz_attest_sign(share.header_hash, share.nonce, share.share_sig) */
    memset(share.share_sig, 0, 64);

    /* Send via LoRa to coordinator */
    /* quartz_lora_send_pool_share(&share); */

    s_pool.state.my_shares++;
    ESP_LOGI(TAG, "Share submitted (height=%u, nonce=%llu)", height, nonce);

    return QZ_OK;
}

qz_err_t quartz_pool_process_share(const qz_pool_share_t *share) {
    if (!s_pool.initialized) return QZ_ERR_NOT_FOUND;
    if (s_pool.mode != QZ_POOL_MODE_COORDINATOR) return QZ_ERR_INVALID;

    /* Find or add member */
    qz_pool_member_t *m = find_member(share->miner_pubkey);
    if (!m) {
        m = add_member(share->miner_pubkey, NULL);
        if (!m) return QZ_ERR_NO_MEM;
    }

    /* Verify share signature (in production) */
    /* quartz_attest_verify_block(share->header_hash, share->nonce, ...) */

    /* Verify share meets pool difficulty */
    /* uint32_t share_int = read_be(share->header_hash); */
    /* if (share_int >= pool_target) return QZ_ERR_INVALID; */

    /* Update member stats */
    m->shares++;
    m->total_shares++;
    m->last_share_block = share->height;
    m->active = true;

    s_pool.state.pool_total_shares++;

    return QZ_OK;
}

qz_err_t quartz_pool_compute_split(
    uint64_t total_reward,
    qz_pool_payout_t *payouts,
    uint8_t max_payouts,
    uint8_t *payout_count
) {
    if (s_pool.mode != QZ_POOL_MODE_COORDINATOR) return QZ_ERR_INVALID;
    if (s_pool.member_count == 0) return QZ_ERR_NOT_FOUND;

    uint32_t total_shares = 0;
    for (int i = 0; i < s_pool.member_count; i++) {
        if (s_pool.members[i].active && s_pool.members[i].shares > 0) {
            total_shares += s_pool.members[i].shares;
        }
    }

    if (total_shares == 0) {
        /* No shares this epoch — pay finder of the block */
        /* (fallback: coordinator gets full reward) */
        if (max_payouts > 0) {
            memcpy(payouts[0].pubkey, quartz_attest_get_pubkey(), 32);
            payouts[0].amount = total_reward;
            *payout_count = 1;
        }
        return QZ_OK;
    }

    uint8_t count = 0;
    uint64_t distributed = 0;

    for (int i = 0; i < s_pool.member_count && count < max_payouts; i++) {
        qz_pool_member_t *m = &s_pool.members[i];
        if (!m->active || m->shares == 0) continue;

        /* Proportional split */
        uint64_t amount = (total_reward * m->shares) / total_shares;
        distributed += amount;

        memcpy(payouts[count].pubkey, m->pubkey, 32);
        payouts[count].amount = amount;
        count++;
    }

    /* Remainder goes to block finder (incentivize finding) */
    if (distributed < total_reward && count > 0 && count < max_payouts) {
        /* In production: add remainder to block finder's output */
    }

    *payout_count = count;

    ESP_LOGI(TAG, "Split %llu sats among %d members (%u total shares)",
             total_reward, count, total_shares);

    return QZ_OK;
}

bool quartz_pool_check_election(
    uint32_t block_height,
    const uint8_t (*candidates)[32],
    uint8_t count
) {
    uint32_t epoch = get_epoch(block_height);

    /* Check if epoch changed */
    if (epoch == s_pool.current_epoch) return false;
    s_pool.current_epoch = epoch;

    /* No candidates = stay solo or keep current coordinator */
    if (count == 0) return false;

    /* Compute election hashes for all candidates */
    uint8_t best_hash[32];
    uint8_t best_idx = 0;
    hash_for_election(candidates[0], epoch, best_hash);

    for (int i = 1; i < count; i++) {
        uint8_t h[32];
        hash_for_election(candidates[i], epoch, h);
        if (memcmp(h, best_hash, 32) < 0) {
            memcpy(best_hash, h, 32);
            best_idx = i;
        }
    }

    /* Is the winner us? */
    const uint8_t *mypub = quartz_attest_get_pubkey();
    if (mypub && memcmp(candidates[best_idx], mypub, 32) == 0) {
        ESP_LOGI(TAG, "Elected coordinator for epoch %u", epoch);
        return true;
    }

    /* Track who won */
    memcpy(s_pool.coordinator_pubkey, candidates[best_idx], 32);
    s_pool.coordinator_known = true;

    return false;
}

bool quartz_pool_coordinator_timeout(uint32_t block_height) {
    if (!s_pool.coordinator_known) return false;

    if (block_height > s_pool.last_coord_block + QZ_POOL_TIMEOUT_BLOCKS) {
        ESP_LOGW(TAG, "Coordinator timeout (no msg since block %u)",
                 s_pool.last_coord_block);
        s_pool.coordinator_known = false;
        return true;
    }

    return false;
}

void quartz_pool_get_stats(qz_pool_stats_t *stats) {
    if (!stats) return;

    stats->mode = s_pool.mode;
    stats->member_count = s_pool.member_count;
    stats->my_shares = s_pool.state.my_shares;
    stats->pool_shares = s_pool.state.pool_total_shares;
    stats->blocks_found = s_pool.state.blocks_found;
    stats->rewards_earned_qz = s_pool.state.rewards_earned / 100000000ULL;
    memcpy(stats->coordinator_pubkey, s_pool.coordinator_pubkey, 6);
    stats->epoch_block = s_pool.current_epoch * QZ_POOL_EPOCH_BLOCKS;
}
