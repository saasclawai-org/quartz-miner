/**
 * quartz_wots.c — WOTS+ Quantum-Resistant Signatures
 *
 * Implementation for ESP32. Uses SHA-256 from mbedtls.
 *
 * The beauty of WOTS+: it's just iterated hashing. No number theory,
 * no elliptic curves, no factoring assumptions. Break SHA-256 and
 * you break everything anyway. A quantum computer with Grover's
 * algorithm gives a quadratic speedup on hash brute-force, but
 * SHA-256 with 128 bits of effective security is still considered
 * safe against quantum attacks.
 *
 * HOW MERKLE TREES MAKE ONE-TIME SIGS REUSABLE:
 * - Generate N=256 WOTS+ keypairs
 * - Each public key is a leaf in a Merkle tree
 * - Address = Merkle root (hash of all leaves)
 * - Signing reveals: WOTS+ sig + auth path from leaf to root
 * - Verifier re-computes leaf from WOTS+ sig, checks path to root
 * - 256 transactions per address, then need new address
 */

#include "quartz_wots.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nvs_flash.h"
#include "mbedtls/sha256.h"
#include "esp_random.h"

static const char *TAG = "QZ.WOTS";
#endif

#define QW_NVS_NS "qz_qwots"
#define QW_KEY_STATE "state"

/* === SHA-256 helpers === */

static void sha256(const uint8_t *data, int len, uint8_t *out) {
#ifdef ESP_PLATFORM
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
#else
    memset(out, 0, 32);
#endif
}

/* XOR two hashes: out = a XOR b */
static void hash_xor(uint8_t *out, const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < QZ_WOTS_HASH_SIZE; i++) {
        out[i] = a[i] ^ b[i];
    }
}

/* === PRF (Pseudo-Random Function) for key derivation === */

/* Derive a WOTS+ private key chain from seed + chain index */
static void prf_chain(
    const uint8_t seed[32],
    uint32_t chain_idx,
    uint8_t out[QZ_WOTS_HASH_SIZE]
) {
    /* SHA-256(seed || chain_idx) */
    uint8_t buf[36];
    memcpy(buf, seed, 32);
    buf[32] = (chain_idx >> 0) & 0xFF;
    buf[33] = (chain_idx >> 8) & 0xFF;
    buf[34] = (chain_idx >> 16) & 0xFF;
    buf[35] = (chain_idx >> 24) & 0xFF;
    sha256(buf, 36, out);
}

/* === WOTS+ hash chain === */

/* Hash a value n times: H^n(input)
 * With optional key for WOTS+ variant (we use simple variant) */
static void hash_chain(
    uint8_t *value,         /* in/out: QZ_WOTS_HASH_SIZE bytes */
    int iterations          /* how many times to hash */
) {
    for (int i = 0; i < iterations; i++) {
        uint8_t tmp[QZ_WOTS_HASH_SIZE];
        sha256(value, QZ_WOTS_HASH_SIZE, tmp);
        memcpy(value, tmp, QZ_WOTS_HASH_SIZE);
    }
}

/* === Base-2^w conversion === */
/* Convert 32-byte message hash into 67 values of w=4 bits each (0-15) */

static void msg_to_chain_values(
    const uint8_t msg_hash[32],
    uint8_t values[QZ_WOTS_TOTAL]
) {
    /* Extract 64 nibbles from 32-byte hash (4 bits each) */
    for (int i = 0; i < 32; i++) {
        values[2 * i]     = (msg_hash[i] >> 4) & 0x0F;     /* high nibble */
        values[2 * i + 1] = msg_hash[i] & 0x0F;             /* low nibble */
    }

    /* Compute checksum: sum of (chain_len - value) for all message values */
    /* Checksum prevents the attacker from forging by extending chains */
    uint32_t checksum = 0;
    for (int i = 0; i < QZ_WOTS_MSG_CHAINS; i++) {
        checksum += QZ_WOTS_CHAIN_LEN - values[i];
    }

    /* Encode checksum as 3 base-16 values (12 bits, enough for 64×15=960 max) */
    values[64] = (checksum >> 8) & 0x0F;
    values[65] = (checksum >> 4) & 0x0F;
    values[66] = checksum & 0x0F;
}

/* === WOTS+ key generation === */

/* Generate private key (67 chains) from seed */
static void wots_gen_privkey(
    const uint8_t seed[32],
    uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE]
) {
    for (int i = 0; i < QZ_WOTS_TOTAL; i++) {
        prf_chain(seed, i, &privkey[i * QZ_WOTS_HASH_SIZE]);
    }
}

/* Generate public key by hashing each chain to full length */
static void wots_gen_pubkey(
    const uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE],
    uint8_t pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE]
) {
    for (int i = 0; i < QZ_WOTS_TOTAL; i++) {
        memcpy(&pubkey[i * QZ_WOTS_HASH_SIZE],
               &privkey[i * QZ_WOTS_HASH_SIZE],
               QZ_WOTS_HASH_SIZE);
        hash_chain(&pubkey[i * QZ_WOTS_HASH_SIZE], QZ_WOTS_CHAIN_LEN);
    }
}

/* === WOTS+ signing === */

static void wots_sign(
    const uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE],
    const uint8_t msg_hash[32],
    uint8_t sig[QZ_WOTS_SIG_SIZE]
) {
    uint8_t values[QZ_WOTS_TOTAL];
    msg_to_chain_values(msg_hash, values);

    for (int i = 0; i < QZ_WOTS_TOTAL; i++) {
        /* sig[i] = H^values[i](privkey[i]) */
        memcpy(&sig[i * QZ_WOTS_HASH_SIZE],
               &privkey[i * QZ_WOTS_HASH_SIZE],
               QZ_WOTS_HASH_SIZE);
        hash_chain(&sig[i * QZ_WOTS_HASH_SIZE], values[i]);
    }
}

/* === WOTS+ verification === */

static void wots_verify_pubkey(
    const uint8_t sig[QZ_WOTS_SIG_SIZE],
    const uint8_t msg_hash[32],
    uint8_t computed_pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE]
) {
    uint8_t values[QZ_WOTS_TOTAL];
    msg_to_chain_values(msg_hash, values);

    for (int i = 0; i < QZ_WOTS_TOTAL; i++) {
        /* Complete the chain: H^(chain_len - values[i])(sig[i]) */
        memcpy(&computed_pubkey[i * QZ_WOTS_HASH_SIZE],
               &sig[i * QZ_WOTS_HASH_SIZE],
               QZ_WOTS_HASH_SIZE);
        int remaining = QZ_WOTS_CHAIN_LEN - values[i];
        hash_chain(&computed_pubkey[i * QZ_WOTS_HASH_SIZE], remaining);
    }
}

/* === Merkle Tree === */

/* Compute the leaf hash for OTS index i: H(pubkey[i]) */
static void merkle_leaf(
    const uint8_t pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE],
    uint8_t leaf[QZ_WOTS_HASH_SIZE]
) {
    sha256(pubkey, QZ_WOTS_PUBKEY_SIZE, leaf);
}

/* Build Merkle tree from 256 leaves, return root */
static void merkle_root_from_leaves(
    uint8_t leaves[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE],
    uint8_t root[QZ_WOTS_HASH_SIZE]
) {
    int n = QZ_MERKLE_LEAVES;
    uint8_t(*cur)[QZ_WOTS_HASH_SIZE] = leaves;

    /* Use stack-based tree computation */
    /* Level 0: hash each leaf: H(leaf || leaf_index) */
    /* We use H(leaf_i) directly for simplicity */
    /* Actually standard Merkle: parent = H(left || right) */

    uint8_t buf[2 * QZ_WOTS_HASH_SIZE];
    uint8_t tmp[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE];

    while (n > 1) {
        for (int i = 0; i < n / 2; i++) {
            memcpy(buf, cur[2 * i], QZ_WOTS_HASH_SIZE);
            memcpy(buf + QZ_WOTS_HASH_SIZE, cur[2 * i + 1], QZ_WOTS_HASH_SIZE);
            sha256(buf, 2 * QZ_WOTS_HASH_SIZE, tmp[i]);
        }
        n /= 2;
        memcpy(cur, tmp, n * QZ_WOTS_HASH_SIZE);
    }

    memcpy(root, cur[0], QZ_WOTS_HASH_SIZE);
}

/* Compute authentication path for leaf at index `leaf_idx` */
static void merkle_auth_path(
    uint8_t leaves[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE],
    int leaf_idx,
    uint8_t auth_path[QZ_MERKLE_HEIGHT * QZ_WOTS_HASH_SIZE]
) {
    int n = QZ_MERKLE_LEAVES;
    uint8_t(*cur)[QZ_WOTS_HASH_SIZE] = leaves;
    uint8_t tmp[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE];
    uint8_t buf[2 * QZ_WOTS_HASH_SIZE];
    int idx = leaf_idx;

    for (int level = 0; level < QZ_MERKLE_HEIGHT; level++) {
        /* Sibling index */
        int sibling = (idx % 2 == 0) ? idx + 1 : idx - 1;
        memcpy(&auth_path[level * QZ_WOTS_HASH_SIZE],
               cur[sibling], QZ_WOTS_HASH_SIZE);

        /* Compute next level */
        for (int i = 0; i < n / 2; i++) {
            memcpy(buf, cur[2 * i], QZ_WOTS_HASH_SIZE);
            memcpy(buf + QZ_WOTS_HASH_SIZE, cur[2 * i + 1], QZ_WOTS_HASH_SIZE);
            sha256(buf, 2 * QZ_WOTS_HASH_SIZE, tmp[i]);
        }
        n /= 2;
        memcpy(cur, tmp, n * QZ_WOTS_HASH_SIZE);
        idx /= 2;
    }
}

/* Verify auth path: compute root from leaf + auth path */
static void merkle_verify_path(
    const uint8_t leaf[QZ_WOTS_HASH_SIZE],
    int leaf_idx,
    const uint8_t auth_path[QZ_MERKLE_HEIGHT * QZ_WOTS_HASH_SIZE],
    uint8_t computed_root[QZ_WOTS_HASH_SIZE]
) {
    uint8_t current[QZ_WOTS_HASH_SIZE];
    memcpy(current, leaf, QZ_WOTS_HASH_SIZE);

    uint8_t buf[2 * QZ_WOTS_HASH_SIZE];

    for (int level = 0; level < QZ_MERKLE_HEIGHT; level++) {
        if (leaf_idx % 2 == 0) {
            /* Current is left child */
            memcpy(buf, current, QZ_WOTS_HASH_SIZE);
            memcpy(buf + QZ_WOTS_HASH_SIZE,
                   &auth_path[level * QZ_WOTS_HASH_SIZE],
                   QZ_WOTS_HASH_SIZE);
        } else {
            /* Current is right child */
            memcpy(buf, &auth_path[level * QZ_WOTS_HASH_SIZE],
                   QZ_WOTS_HASH_SIZE);
            memcpy(buf + QZ_WOTS_HASH_SIZE, current, QZ_WOTS_HASH_SIZE);
        }
        sha256(buf, 2 * QZ_WOTS_HASH_SIZE, current);
        leaf_idx /= 2;
    }

    memcpy(computed_root, current, QZ_WOTS_HASH_SIZE);
}

/* === NVS persistence === */

#ifdef ESP_PLATFORM
static bool save_wallet_state(const qz_qwallet_t *wallet, const uint8_t seed[32]) {
    nvs_handle_t handle;
    if (nvs_open(QW_NVS_NS, NVS_READWRITE, &handle) != ESP_OK) return false;

    /* Store: seed (32) + merkle_root (32) + next_ots (2) */
    uint8_t blob[66];
    memcpy(blob, seed, 32);
    memcpy(blob + 32, wallet->merkle_root, 32);
    blob[64] = wallet->next_ots_index & 0xFF;
    blob[65] = (wallet->next_ots_index >> 8) & 0xFF;

    esp_err_t err = nvs_set_blob(handle, QW_KEY_STATE, blob, sizeof(blob));
    nvs_commit(handle);
    nvs_close(handle);
    return (err == ESP_OK);
}

static bool load_wallet_state(uint8_t seed[32], qz_qwallet_t *wallet) {
    nvs_handle_t handle;
    if (nvs_open(QW_NVS_NS, NVS_READONLY, &handle) != ESP_OK) return false;

    uint8_t blob[66];
    size_t needed = sizeof(blob);
    esp_err_t err = nvs_get_blob(handle, QW_KEY_STATE, blob, &needed);
    nvs_close(handle);

    if (err != ESP_OK || needed != sizeof(blob)) return false;

    memcpy(seed, blob, 32);
    memcpy(wallet->merkle_root, blob + 32, 32);
    wallet->next_ots_index = blob[64] | (blob[65] << 8);
    wallet->max_ots_index = QZ_MERKLE_LEAVES;
    wallet->rotation_mode = false;
    wallet->initialized = true;
    return true;
}
#endif

/* === Public API === */

int quartz_qwallet_create(const uint8_t seed[32], qz_qwallet_t *wallet) {
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Generating quantum wallet (256 WOTS+ keypairs, Merkle height %d)...",
             QZ_MERKLE_HEIGHT);

    /* We need to generate all 256 OTS keypairs to compute the Merkle root.
     * Each OTS pubkey is 2144 bytes. We can't hold all 256 at once (548KB > SRAM).
     * Strategy: compute leaf hashes one at a time. */

    uint8_t leaves[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE];
    uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    uint8_t pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];

    /* Derive per-OTS seeds: seed_i = SHA256(master_seed || i) */
    for (int k = 0; k < QZ_MERKLE_LEAVES; k++) {
        uint8_t ots_seed[32];
        uint8_t buf[36];
        memcpy(buf, seed, 32);
        buf[32] = k & 0xFF;
        buf[33] = (k >> 8) & 0xFF;
        buf[34] = 0;
        buf[35] = 0;
        sha256(buf, 36, ots_seed);

        /* Generate OTS keypair */
        wots_gen_privkey(ots_seed, privkey);
        wots_gen_pubkey(privkey, pubkey);

        /* Leaf = H(pubkey) */
        merkle_leaf(pubkey, leaves[k]);

        if (k % 32 == 0) {
            ESP_LOGI(TAG, "  Generated OTS keypair %d/%d", k, QZ_MERKLE_LEAVES);
        }
    }

    /* Compute Merkle root */
    merkle_root_from_leaves(leaves, wallet->merkle_root);
    wallet->next_ots_index = 0;
    wallet->max_ots_index = QZ_MERKLE_LEAVES;
    wallet->rotation_mode = false;
    wallet->initialized = true;

    /* Save to NVS */
    if (!save_wallet_state(wallet, seed)) {
        ESP_LOGE(TAG, "Failed to save quantum wallet to NVS");
        return -1;
    }

    ESP_LOGI(TAG, "Quantum wallet created: %d OTS keys, root stored",
             QZ_MERKLE_LEAVES);
    return 0;
#else
    memset(wallet->merkle_root, 0, QZ_QADDR_SIZE);
    wallet->next_ots_index = 0;
    wallet->max_ots_index = QZ_MERKLE_LEAVES;
    wallet->rotation_mode = false;
    wallet->initialized = true;
    return 0;
#endif
}

int quartz_qwallet_load(qz_qwallet_t *wallet) {
#ifdef ESP_PLATFORM
    uint8_t seed[32];
    if (load_wallet_state(seed, wallet)) {
        ESP_LOGI(TAG, "Quantum wallet loaded: OTS %d/%d used",
                 wallet->next_ots_index, wallet->max_ots_index);
        return 0;
    }
    return -1;
#else
    wallet->next_ots_index = 0;
    wallet->max_ots_index = QZ_MERKLE_LEAVES;
    wallet->rotation_mode = false;
    wallet->initialized = true;
    return 0;
#endif
}

int quartz_qwallet_sign(
    const qz_qwallet_t *wallet,
    const uint8_t msg_hash[32],
    uint8_t *sig_out,
    size_t *sig_len
) {
#ifdef ESP_PLATFORM
    /* Regular signing: refuse if only the reserved rotation sig is left */
    int total_left = wallet->max_ots_index - wallet->next_ots_index;
    if (!wallet->initialized || total_left <= QZ_RESERVED_ROTATION_SIG) {
        ESP_LOGE(TAG, "No spendable OTS keys remaining (used %d/%d, %d reserved for rotation)",
                 wallet->next_ots_index, wallet->max_ots_index, QZ_RESERVED_ROTATION_SIG);
        ESP_LOGE(TAG, "Use key rotation to transfer funds to a new address!");
        return -1;
    }

    int ots_idx = wallet->next_ots_index;
    ESP_LOGI(TAG, "Signing with OTS key %d/%d", ots_idx, wallet->max_ots_index);

    /* Load seed from NVS */
    uint8_t seed[32];
    qz_qwallet_t tmp_wallet;
    if (!load_wallet_state(seed, &tmp_wallet)) {
        ESP_LOGE(TAG, "Failed to load seed");
        return -1;
    }

    /* Derive this OTS keypair's seed */
    uint8_t ots_seed[32];
    uint8_t buf[36];
    memcpy(buf, seed, 32);
    buf[32] = ots_idx & 0xFF;
    buf[33] = (ots_idx >> 8) & 0xFF;
    buf[34] = 0;
    buf[35] = 0;
    sha256(buf, 36, ots_seed);

    /* Generate private key */
    uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    wots_gen_privkey(ots_seed, privkey);

    /* Create WOTS+ signature */
    uint8_t *wots_sig = sig_out;
    wots_sign(privkey, msg_hash, wots_sig);

    /* Compute Merkle auth path */
    /* We need all leaves for auth path, compute them one by one */
    uint8_t leaves[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE];
    uint8_t pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    uint8_t privkey_tmp[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];

    /* This is memory-intensive but we need the full leaf set.
     * 256 leaves × 32 bytes = 8KB — fine for ESP32. */
    for (int k = 0; k < QZ_MERKLE_LEAVES; k++) {
        uint8_t k_seed[32];
        memcpy(buf, seed, 32);
        buf[32] = k & 0xFF;
        buf[33] = (k >> 8) & 0xFF;
        sha256(buf, 36, k_seed);
        wots_gen_privkey(k_seed, privkey_tmp);
        wots_gen_pubkey(privkey_tmp, pubkey);
        merkle_leaf(pubkey, leaves[k]);
    }

    /* Auth path goes after WOTS+ sig */
    uint8_t *auth = sig_out + QZ_WOTS_SIG_SIZE;
    merkle_auth_path(leaves, ots_idx, auth);

    /* OTS index (4 bytes, little-endian) */
    uint32_t idx_le = ots_idx;
    memcpy(sig_out + QZ_WOTS_SIG_SIZE + QZ_MERKLE_AUTH_SIZE, &idx_le, 4);

    *sig_len = QZ_QSIG_SIZE;

    ESP_LOGI(TAG, "Quantum signature: %d bytes (WOTS=%d + auth=%d + idx=4)",
             (int)*sig_len, QZ_WOTS_SIG_SIZE, QZ_MERKLE_AUTH_SIZE);

    /* Advance OTS index in NVS */
    ((qz_qwallet_t *)wallet)->next_ots_index++;
    save_wallet_state(wallet, seed);

    return 0;
#else
    *sig_len = QZ_QSIG_SIZE;
    memset(sig_out, 0, *sig_len);
    return 0;
#endif
}

int quartz_qwallet_sign_rotation(
    const qz_qwallet_t *wallet,
    const uint8_t msg_hash[32],
    uint8_t *sig_out,
    size_t *sig_len
) {
#ifdef ESP_PLATFORM
    /* Rotation signing: uses the reserved last OTS key (#255 = 256th sig) */
    int total_left = wallet->max_ots_index - wallet->next_ots_index;
    if (!wallet->initialized || total_left < 1) {
        ESP_LOGE(TAG, "No OTS keys remaining at all (used %d/%d) — funds may be stuck!",
                 wallet->next_ots_index, wallet->max_ots_index);
        return -1;
    }
    if (total_left > QZ_RESERVED_ROTATION_SIG) {
        ESP_LOGW(TAG, "Rotation called early (used %d/%d) — continuing anyway",
                 wallet->next_ots_index, wallet->max_ots_index);
    }

    ESP_LOGI(TAG, "KEY ROTATION: signing self-transfer with OTS key %d/%d (reserved)",
             wallet->next_ots_index, wallet->max_ots_index);

    /* Mark rotation mode */
    ((qz_qwallet_t *)wallet)->rotation_mode = true;

    /* Same signing logic as regular sign */
    int ots_idx = wallet->next_ots_index;

    /* Load seed from NVS */
    uint8_t seed[32];
    qz_qwallet_t tmp_wallet;
    if (!load_wallet_state(seed, &tmp_wallet)) {
        ESP_LOGE(TAG, "Failed to load seed");
        return -1;
    }

    /* Derive this OTS keypair's seed */
    uint8_t ots_seed[32];
    uint8_t buf[36];
    memcpy(buf, seed, 32);
    buf[32] = ots_idx & 0xFF;
    buf[33] = (ots_idx >> 8) & 0xFF;
    buf[34] = 0;
    buf[35] = 0;
    sha256(buf, 36, ots_seed);

    /* Generate private key */
    uint8_t privkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    wots_gen_privkey(ots_seed, privkey);

    /* Create WOTS+ signature */
    uint8_t *wots_sig = sig_out;
    wots_sign(privkey, msg_hash, wots_sig);

    /* Compute Merkle auth path */
    uint8_t leaves[QZ_MERKLE_LEAVES][QZ_WOTS_HASH_SIZE];
    uint8_t pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    uint8_t privkey_tmp[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];

    for (int k = 0; k < QZ_MERKLE_LEAVES; k++) {
        uint8_t k_seed[32];
        memcpy(buf, seed, 32);
        buf[32] = k & 0xFF;
        buf[33] = (k >> 8) & 0xFF;
        sha256(buf, 36, k_seed);
        wots_gen_privkey(k_seed, privkey_tmp);
        wots_gen_pubkey(privkey_tmp, pubkey);
        merkle_leaf(pubkey, leaves[k]);
    }

    uint8_t *auth = sig_out + QZ_WOTS_SIG_SIZE;
    merkle_auth_path(leaves, ots_idx, auth);

    uint32_t idx_le = ots_idx;
    memcpy(sig_out + QZ_WOTS_SIG_SIZE + QZ_MERKLE_AUTH_SIZE, &idx_le, 4);

    *sig_len = QZ_QSIG_SIZE;

    ESP_LOGI(TAG, "Rotation signature: %d bytes — FUNDS MUST BE MOVED TO NEW ADDRESS NOW",
             (int)*sig_len);

    /* Advance OTS index in NVS — wallet is now exhausted */
    ((qz_qwallet_t *)wallet)->next_ots_index++;
    save_wallet_state(wallet, seed);

    return 0;
#else
    *sig_len = QZ_QSIG_SIZE;
    memset(sig_out, 0, *sig_len);
    return 0;
#endif
}

int quartz_qwallet_verify(
    const uint8_t merkle_root[QZ_QADDR_SIZE],
    const uint8_t msg_hash[32],
    const uint8_t *sig,
    size_t sig_len
) {
    if (sig_len != QZ_QSIG_SIZE) return -1;

    /* Extract OTS index */
    uint32_t idx;
    memcpy(&idx, sig + QZ_WOTS_SIG_SIZE + QZ_MERKLE_AUTH_SIZE, 4);
    if (idx >= QZ_MERKLE_LEAVES) return -1;

    /* Step 1: Complete WOTS+ chains to get reconstructed pubkey */
    uint8_t computed_pubkey[QZ_WOTS_TOTAL * QZ_WOTS_HASH_SIZE];
    wots_verify_pubkey(sig, msg_hash, computed_pubkey);

    /* Step 2: Hash pubkey to get leaf */
    uint8_t leaf[QZ_WOTS_HASH_SIZE];
    merkle_leaf(computed_pubkey, leaf);

    /* Step 3: Verify Merkle auth path */
    const uint8_t *auth = sig + QZ_WOTS_SIG_SIZE;
    uint8_t computed_root[QZ_WOTS_HASH_SIZE];
    merkle_verify_path(leaf, idx, auth, computed_root);

    /* Step 4: Compare to expected root */
    if (memcmp(computed_root, merkle_root, QZ_WOTS_HASH_SIZE) == 0) {
        return 0;  /* Valid signature */
    }

    return -1;  /* Invalid */
}

/* === Address helpers === */

static char s_addr_hex[65] = {0};

const char *quartz_qwallet_address_hex(const qz_qwallet_t *wallet) {
    for (int i = 0; i < QZ_QADDR_SIZE; i++) {
        sprintf(s_addr_hex + i * 2, "%02x", wallet->merkle_root[i]);
    }
    s_addr_hex[64] = '\0';
    return s_addr_hex;
}

int quartz_qwallet_remaining(const qz_qwallet_t *wallet) {
    if (!wallet->initialized) return 0;
    int total = wallet->max_ots_index - wallet->next_ots_index;
    /* Subtract reserved rotation signature — can't spend it normally */
    if (total > QZ_RESERVED_ROTATION_SIG)
        return total - QZ_RESERVED_ROTATION_SIG;
    return 0;  /* only the reserved sig left, or none */
}

int quartz_qwallet_remaining_total(const qz_qwallet_t *wallet) {
    if (!wallet->initialized) return 0;
    return wallet->max_ots_index - wallet->next_ots_index;
}

int quartz_qwallet_rotation_status(const qz_qwallet_t *wallet) {
    if (!wallet->initialized) return 0;
    if (wallet->next_ots_index >= 254)
        return 2;  /* urgent: only regular sig + reserved left */
    if (wallet->next_ots_index >= 240)
        return 1;  /* warn: rotation coming soon */
    return 0;
}

bool quartz_qwallet_needs_rotation(const qz_qwallet_t *wallet) {
    return quartz_qwallet_rotation_status(wallet) >= 1;
}
