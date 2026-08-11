/**
 * Quartz LoRa Mesh Networking
 *
 * LoRa provides long-range (2-15km) device-to-device communication
 * without internet. ESP32 miners form a mesh that gossips block
 * headers and relays transactions.
 *
 * Radio layers:
 *   BLE    — phone ↔ ESP32 (pairing, signing, short range)
 *   WiFi   — ESP32 ↔ internet (full block sync, node API)
 *   LoRa   — ESP32 ↔ ESP32 (mesh gossip, offline mining, tx relay)
 *
 * LoRa mesh protocol:
 *   - Block headers (80 bytes) broadcast on new block
 *   - New transactions relayed as thin packets
 *   - Peer discovery beacons every 60s
 *   - Store-and-forward for offline peers
 *   - Flooding gossip with TTL decrement
 *
 * Frequency plans (region-specific):
 *   EU 868 MHz  — 1% duty cycle, +14 dBm
 *   US 915 MHz  — FCC part 15, +30 dBm
 *   AS 923 MHz  — Various Asian countries
 *   CN 470 MHz  — China
 *   IN 865 MHz  — India
 */

#ifndef QUARTZ_LORA_H
#define QUARTZ_LORA_H

#include "quartz.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// Configuration
// ============================================================

#define QZ_LORA_FREQ_EU868      868100000   // EU default
#define QZ_LORA_FREQ_US915      915000000   // US default
#define QZ_LORA_FREQ_AS923      923200000   // Asia default
#define QZ_LORA_FREQ_CN470      470000000   // China default
#define QZ_LORA_FREQ_IN865      865000000   // India default

#define QZ_LORA_BANDWIDTH       125000      // 125 kHz
#define QZ_LORA_SPREADING_FACTOR 7         // SF7 — fastest, ~2km range
#define QZ_LORA_CODING_RATE     5           // 4/5 coding rate
#define QZ_LORA_TX_POWER        14          // +14 dBm (EU max)
#define QZ_LORA_PREAMBLE_LEN    8
#define QZ_LORA_SYNC_WORD       0xQU        // Quartz sync word

// Mesh parameters
#define QZ_LORA_MAX_PEERS       32          // track up to 32 mesh peers
#define QZ_LORA_BEACON_INTERVAL_SEC   60    // announce ourselves every 60s
#define QZ_LORA_PEER_TIMEOUT_SEC      300   // peer considered stale after 5 min
#define QZ_LORA_TX_BUFFER_SIZE   242       // max LoRa packet (EU 868)
#define QZ_LORA_GOSSIP_TTL       7         // mesh hop limit
#define QZ_LORA_STORE_FORWARD   16         // store last 16 packets for late peers

// ============================================================
// Packet Types
// ============================================================

typedef enum {
    QZ_LORA_PKT_INVALID     = 0,
    QZ_LORA_PKT_BEACON      = 1,   // "I'm here" — peer discovery
    QZ_LORA_PKT_BLOCK_HDR   = 2,   // 80-byte block header announcement
    QZ_LORA_PKT_TX          = 3,   // Transaction relay (up to 200 bytes)
    QZ_LORA_PKT_BLOCK_REQ   = 4,   // "I need block #N" — store-and-forward
    QZ_LORA_PKT_BLOCK_DATA  = 5,   // Chunked block data response
    QZ_LORA_PKT_PEER_LIST   = 6,   // Known peers exchange (mesh routing)
    QZ_LORA_PKT_PING        = 7,
    QZ_LORA_PKT_PONG        = 8,
} qz_lora_pkt_type_t;

// ============================================================
// Packet Structure (fits in 242 bytes)
// ============================================================

// All LoRa packets have this 8-byte header
typedef struct __attribute__((packed)) {
    uint8_t  type;         // qz_lora_pkt_type_t
    uint8_t  ttl;          // decremented at each hop, dropped at 0
    uint8_t  sender_id[6]; // ESP32 MAC address (last 6 bytes)
    uint8_t  hop_count;    // incremented at each forward
    uint8_t  reserved;     // alignment
    uint16_t payload_len;  // payload length in bytes
} qz_lora_header_t;

#define QZ_LORA_HEADER_SIZE   8
#define QZ_LORA_MAX_PAYLOAD   (QZ_LORA_TX_BUFFER_SIZE - QZ_LORA_HEADER_SIZE)

// Beacon payload (12 bytes)
typedef struct __attribute__((packed)) {
    uint32_t current_height;   // latest block height this node knows
    uint8_t  current_hash[8];  // first 8 bytes of best block hash
    uint8_t  capabilities;     // bit 0: has WiFi, bit 1: has full node
    uint8_t  mesh_id;          // mesh network ID (0 = default)
} qz_lora_beacon_t;

// Block header announcement (80 bytes = fits in one packet)
typedef struct __attribute__((packed)) {
    uint32_t version;
    uint32_t height;
    uint8_t  prev_block_hash[32];
    uint8_t  merkle_root[32];
    uint32_t timestamp;
    uint32_t difficulty_bits;
    uint32_t nonce;
} qz_lora_block_announce_t;

// Transaction relay (variable, up to 200 bytes)
typedef struct __attribute__((packed)) {
    uint8_t  tx_type;        // 0 = transfer, 1 = coinbase
    uint64_t amount;         // in quartz-sats
    uint8_t  sender[32];     // Ed25519 public key
    uint8_t  recipient[25];  // Quartz address bytes
    uint8_t  signature[64];  // Ed25519 signature
    uint64_t fee;            // in quartz-sats
    // Total: 158 bytes — fits in one LoRa packet
} qz_lora_tx_relay_t;

// ============================================================
// Peer Tracking
// ============================================================

typedef struct {
    uint8_t  mac[6];
    uint32_t last_seen_sec;     // uptime when last heard from
    uint32_t known_height;      // their best block height
    uint8_t  capabilities;
    bool     active;
} qz_lora_peer_t;

// ============================================================
// API
// ============================================================

/**
 * Initialize LoRa radio (SX1262 or SX1276).
 * Auto-detects chip on Heltec/TTGO boards.
 *
 * @param freq_hz  Frequency in Hz (use QZ_LORA_FREQ_* constants)
 * @return QZ_OK on success
 */
int quartz_lora_init(uint32_t freq_hz);

/**
 * Send a packet over LoRa mesh.
 * Packet is broadcast to all peers in range.
 * Peers re-broadcast (flooding gossip) with TTL decrement.
 *
 * @param type     Packet type
 * @param payload  Payload data
 * @param len      Payload length (max 234 bytes)
 * @return QZ_OK on success
 */
int quartz_lora_send(qz_lora_pkt_type_t type, const uint8_t *payload, size_t len);

/**
 * Receive loop — call from a FreeRTOS task.
 * Blocks until packet received, then dispatches to handler.
 */
void quartz_lora_receive_task(void *arg);

/**
 * Broadcast a beacon — announces our presence to the mesh.
 * Called automatically every 60 seconds.
 */
int quartz_lora_beacon(void);

/**
 * Announce a newly found block to the mesh.
 * Sends block header to all peers via flooding gossip.
 */
int quartz_lora_announce_block(const qz_lora_block_announce_t *block);

/**
 * Relay a transaction to the mesh.
 */
int quartz_lora_relay_tx(const qz_lora_tx_relay_t *tx);

/**
 * Get list of known mesh peers.
 */
const qz_lora_peer_t *quartz_lora_get_peers(uint8_t *count);

/**
 * Set mesh region frequency.
 * Must be called before init.
 */
void quartz_lora_set_region(uint32_t freq_hz);

/**
 * Callback — called when a block header is received via LoRa.
 * Node can then request full block via WiFi if available,
 * or wait for chunked data via LoRa store-and-forward.
 */
typedef void (*qz_lora_block_cb)(const qz_lora_block_announce_t *block, const uint8_t *from_mac);

/**
 * Callback — called when a transaction is received via LoRa.
 */
typedef void (*qz_lora_tx_cb)(const qz_lora_tx_relay_t *tx, const uint8_t *from_mac);

/**
 * Register callbacks.
 */
void quartz_lora_on_block(qz_lora_block_cb cb);
void quartz_lora_on_tx(qz_lora_tx_cb cb);

/**
 * Statistics
 */
typedef struct {
    uint32_t packets_sent;
    uint32_t packets_received;
    uint32_t packets_relayed;
    uint32_t blocks_announced;
    uint32_t tx_relayed;
    int8_t   last_rssi;
    float    last_snr;
    uint8_t  active_peers;
} qz_lora_stats_t;

void quartz_lora_get_stats(qz_lora_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_LORA_H
