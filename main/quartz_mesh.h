/**
 * quartz_mesh.h — ESP-NOW Mesh Networking for Quartz Miners
 *
 * Boards within WiFi range discover each other via ESP-NOW broadcast
 * and relay mining data peer-to-peer:
 *
 *   - WORK:  A board with WiFi shares block templates with offline peers
 *   - FOUND: A board that found a block relays it to a connected peer for submission
 *   - SYNC:  Periodic peer list sync (who's online, who has WiFi)
 *
 * Topology: flat mesh. Every board broadcasts on a fixed ESP-NOW channel.
 * No routing — direct peer-to-peer within radio range (~200m line-of-sight).
 *
 * Channel: 1 (same as ESP-NOW test bench)
 * Max peers: 8 (ESP-NOW hardware limit is ~20, 8 is sane for nearby miners)
 * Payload:  up to 250 bytes per ESP-NOW packet (multi-packet for block headers)
 *
 * License: MIT
 */

#ifndef QUARTZ_MESH_H
#define QUARTZ_MESH_H

#include <stdint.h>
#include <stdbool.h>
#include "quartz_wifi.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Mesh message types (1 byte packet ID)
 * ============================================================ */

#define QZ_MESH_HELLO       0x01   /* Discovery: "I'm here" — MAC + capabilities */
#define QZ_MESH_WORK        0x02   /* Block template relay (80B header + target + height + job_id) */
#define QZ_MESH_FOUND      0x03   /* Block found: header + nonce — relay to connected peer */
#define QZ_MESH_PEER_LIST   0x04   /* Peer list sync */
#define QZ_MESH_ACK         0x05   /* Acknowledgement */

/* Capabilities flags (in HELLO) */
#define QZ_CAP_HAS_WIFI    0x01   /* This board has WiFi connection to node */
#define QZ_CAP_IS_MINING   0x02   /* This board is actively mining */

/* ============================================================
 * Data structures
 * ============================================================ */

/* Mesh peer entry */
typedef struct {
    uint8_t  mac[6];
    uint8_t  caps;         /* QZ_CAP_* flags */
    uint32_t last_seen;    /* Unix-ish timestamp (uptime seconds) */
    uint32_t height;       /* Last known block height from this peer */
} qz_mesh_peer_t;

/* Work packet (fits in single ESP-NOW payload: 1 + 80 + 4 + 4 + 32 = 121 bytes) */
typedef struct {
    uint8_t  type;           /* QZ_MESH_WORK */
    uint8_t  header[80];     /* Block header */
    uint32_t target_bits;    /* Difficulty target */
    uint32_t height;         /* Block height */
    char     job_id[32];     /* Job ID from server */
} __attribute__((packed)) qz_mesh_work_t;

/* Found-block packet (1 + 80 + 8 = 89 bytes) */
typedef struct {
    uint8_t  type;          /* QZ_MESH_FOUND */
    uint8_t  header[80];    /* Block header that was mined */
    uint64_t nonce;         /* Nonce that met target */
} __attribute__((packed)) qz_mesh_found_t;

/* Hello packet (1 + 6 + 1 + 4 = 12 bytes) */
typedef struct {
    uint8_t  type;          /* QZ_MESH_HELLO */
    uint8_t  mac[6];        /* Sender MAC */
    uint8_t  caps;          /* Capabilities */
    uint32_t height;       /* Current block height */
} __attribute__((packed)) qz_mesh_hello_t;

/* ============================================================
 * API
 * ============================================================ */

/**
 * Initialize ESP-NOW mesh.
 * Must be called AFTER WiFi is initialized.
 * Returns QZ_OK on success.
 */
int quartz_mesh_init(void);

/**
 * Broadcast current work to mesh peers.
 * Called when we get fresh work from the node.
 */
void quartz_mesh_share_work(const qz_block_template_t *tmpl);

/**
 * Broadcast a found block to mesh peers.
 * Called when we find a block (for relay to connected peers).
 */
void quartz_mesh_share_found(const uint8_t header[80], uint64_t nonce);

/**
 * Check if we received work from a mesh peer.
 * Returns 0 and fills tmpl if work is available, -1 otherwise.
 * Consumes the work (subsequent calls return -1 until new work arrives).
 */
int quartz_mesh_get_work(qz_block_template_t *tmpl);

/**
 * Check if we received a found block from a mesh peer.
 * Returns 0 and fills header/nonce if available, -1 otherwise.
 * The caller should submit this to the node on behalf of the peer.
 */
int quartz_mesh_get_found(uint8_t header[80], uint64_t *nonce);

/**
 * Update our capabilities (re-broadcast HELLO with new caps).
 */
void quartz_mesh_update_caps(uint8_t caps);

/**
 * Get peer list for display/stats.
 * Returns peer count, fills peers array (max 8).
 */
int quartz_mesh_get_peers(qz_mesh_peer_t *peers, int max_count);

/**
 * Check if mesh is initialized and running.
 */
bool quartz_mesh_is_active(void);

/**
 * Periodic maintenance — call every ~10 seconds from main loop.
 * Prunes stale peers, re-broadcasts HELLO.
 */
void quartz_mesh_step(uint32_t uptime_sec);

#ifdef __cplusplus
}
#endif

#endif /* QUARTZ_MESH_H */