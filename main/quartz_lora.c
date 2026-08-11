/**
 * Quartz LoRa Mesh Implementation — Native SX1276 Driver
 *
 * No RadioLib, no Arduino. Uses our register-level SX1276 driver.
 *
 * Supported hardware:
 *   - LilyGO TTGO LoRa32 V1.6.1 (ESP32 + SX1276, 868/915MHz)
 *   - Heltec WiFi LoRa 32 V2 (ESP32 + SX1276)
 *   - RFM95W breakouts
 *
 * Pin configuration is passed at init time.
 *
 * Mesh protocol features:
 *   - Block header gossip (80 bytes compressed)
 *   - Transaction relay
 *   - Peer discovery beacons
 *   - Store-and-forward for offline peers
 *   - TTL-based flooding with random backoff
 *   - Seen-dedup via sender MAC + seq number
 *
 * License: MIT
 */

#include "quartz_lora.h"
#include "quartz_sx1276.h"
#include "quartz_wallet.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "QUARTZ_LORA";

// ============================================================
// State
// ============================================================

static bool s_lora_initialized = false;
static uint32_t s_current_freq = QZ_LORA_FREQ_EU868;
static qz_lora_peer_t s_peers[QZ_LORA_MAX_PEERS];
static qz_lora_stats_t s_stats = {0};
static qz_lora_block_cb s_block_cb = NULL;
static qz_lora_tx_cb s_tx_cb = NULL;

// Store-and-forward buffer
typedef struct {
    uint8_t data[QZ_LORA_TX_BUFFER_SIZE];
    size_t len;
    uint32_t timestamp;
} stored_packet_t;

static stored_packet_t s_store_fwd[QZ_LORA_STORE_FORWARD];
static int s_store_fwd_idx = 0;

// Seen-packet dedup (circular buffer of hashes)
#define SEEN_TABLE_SIZE 32
static uint32_t s_seen[SEEN_TABLE_SIZE];  // hash of sender_id + seq
static int s_seen_idx = 0;

static bool packet_seen(const uint8_t *sender_id, uint16_t seq) {
    uint32_t h = (sender_id[5] << 24) | (sender_id[4] << 16) | (sender_id[3] << 8) | seq;
    for (int i = 0; i < SEEN_TABLE_SIZE; i++) {
        if (s_seen[i] == h) return true;
    }
    s_seen[s_seen_idx] = h;
    s_seen_idx = (s_seen_idx + 1) % SEEN_TABLE_SIZE;
    return false;
}

// Static sequence counter
static uint16_t s_seq_counter = 0;

// ============================================================
// Default configs for known boards
// ============================================================

// LilyGO TTGO LoRa32 V1.6.1 — SX1276 on VSPI
static const sx1276_config_t T3_CONFIG = {
    .spi_host = VSPI_HOST,     // 3
    .pin_sck  = 5,
    .pin_miso = 19,
    .pin_mosi = 27,
    .pin_cs   = 18,
    .pin_rst  = 14,
    .pin_dio0 = 26,
    .pin_dio1 = 33,

    .freq_hz         = QZ_LORA_FREQ_EU868,
    .bandwidth       = SX1276_BW_125_0,
    .spreading_factor = 7,
    .coding_rate     = SX1276_CR_4_5,
    .sync_word       = 0x12,   // LoRa standard (0x34 for network-private)
    .tx_power_dbm    = 14,
    .preamble_len    = 8,
    .crc_on          = true,
    .implicit_header = false,
    .payload_len     = 0,
};

// ============================================================
// Init
// ============================================================

int quartz_lora_init(uint32_t freq_hz) {
    s_current_freq = freq_hz;

    sx1276_config_t cfg = T3_CONFIG;
    cfg.freq_hz = freq_hz;

    int ret = sx1276_init(&cfg);
    if (ret != 0) {
        ESP_LOGE(TAG, "SX1276 init failed: %d", ret);
        return -1;
    }

    s_lora_initialized = true;
    memset(s_peers, 0, sizeof(s_peers));
    memset(s_seen, 0, sizeof(s_seen));
    memset(&s_stats, 0, sizeof(s_stats));

    ESP_LOGI(TAG, "LoRa mesh initialized: %lu Hz, SF%d, BW 125kHz, TX %d dBm",
             (unsigned long)freq_hz, QZ_LORA_SPREADING_FACTOR, QZ_LORA_TX_POWER);
    ESP_LOGI(TAG, "Expected range: 2-5km urban, 10-15km line-of-sight");

    return 0;
}

void quartz_lora_set_region(uint32_t freq_hz) {
    s_current_freq = freq_hz;
    if (s_lora_initialized) {
        sx1276_set_frequency(freq_hz);
    }
}

// ============================================================
// Packet Send
// ============================================================

int quartz_lora_send(qz_lora_pkt_type_t type, const uint8_t *payload, size_t len) {
    if (!s_lora_initialized) return -1;
    if (len > QZ_LORA_MAX_PAYLOAD) {
        ESP_LOGW(TAG, "Packet too large: %d bytes (max %d)", len, QZ_LORA_MAX_PAYLOAD);
        return -2;
    }

    // Build packet with mesh header
    uint8_t packet[QZ_LORA_TX_BUFFER_SIZE];
    qz_lora_header_t *hdr = (qz_lora_header_t *)packet;
    hdr->type = type;
    hdr->ttl = QZ_LORA_GOSSIP_TTL;
    hdr->hop_count = 0;
    hdr->reserved = 0;
    hdr->payload_len = len;
    hdr->seq = ++s_seq_counter;

    // Get our MAC for sender_id
    uint8_t mac[6] = {0};
    if (s_lora_initialized) {
        // Use WiFi MAC as device identifier (even if WiFi off, it's our identity)
        esp_wifi_get_mac(WIFI_IF_STA, mac);
    }

    // Copy payload
    memcpy(packet + QZ_LORA_HEADER_SIZE, payload, len);
    size_t total_len = QZ_LORA_HEADER_SIZE + len;

    // Send via SX1276
    int ret = sx1276_transmit(packet, total_len);
    if (ret != 0) {
        ESP_LOGW(TAG, "LoRa TX failed: %d", ret);
        return -3;
    }

    s_stats.packets_sent++;

    // Store for late peer replay
    memcpy(s_store_fwd[s_store_fwd_idx].data, packet, total_len);
    s_store_fwd[s_store_fwd_idx].len = total_len;
    s_store_fwd[s_store_fwd_idx].timestamp = esp_timer_get_time() / 1000000;
    s_store_fwd_idx = (s_store_fwd_idx + 1) % QZ_LORA_STORE_FORWARD;

    ESP_LOGD(TAG, "TX type=%d len=%d seq=%d", type, total_len, hdr->seq);
    return 0;
}

// ============================================================
// Beacon
// ============================================================

int quartz_lora_beacon(void) {
    qz_lora_beacon_t beacon = {0};
    beacon.current_height = quartz_chain_get_height();
    const uint8_t *best_hash = quartz_chain_get_best_hash();
    if (best_hash) memcpy(beacon.current_hash, best_hash, 8);
    beacon.capabilities = 0x01; // bit 0: has WiFi
    beacon.mesh_id = 0;

    return quartz_lora_send(QZ_LORA_PKT_BEACON, (uint8_t *)&beacon, sizeof(beacon));
}

// ============================================================
// Block Announcement
// ============================================================

int quartz_lora_announce_block(const qz_lora_block_announce_t *block) {
    int ret = quartz_lora_send(QZ_LORA_PKT_BLOCK_HDR, (const uint8_t *)block, sizeof(*block));
    if (ret == 0) s_stats.blocks_announced++;
    return ret;
}

// ============================================================
// Transaction Relay
// ============================================================

int quartz_lora_relay_tx(const qz_lora_tx_relay_t *tx) {
    int ret = quartz_lora_send(QZ_LORA_PKT_TX, (const uint8_t *)tx, sizeof(*tx));
    if (ret == 0) s_stats.tx_relayed++;
    return ret;
}

// ============================================================
// Peer Management
// ============================================================

static qz_lora_peer_t *find_or_create_peer(const uint8_t *mac) {
    for (int i = 0; i < QZ_LORA_MAX_PEERS; i++) {
        if (s_peers[i].active && memcmp(s_peers[i].mac, mac, 6) == 0) {
            return &s_peers[i];
        }
    }
    for (int i = 0; i < QZ_LORA_MAX_PEERS; i++) {
        if (!s_peers[i].active) {
            memcpy(s_peers[i].mac, mac, 6);
            s_peers[i].active = true;
            s_peers[i].first_seen_sec = esp_timer_get_time() / 1000000;
            ESP_LOGI(TAG, "New peer discovered: %02X:%02X:%02X:%02X:%02X:%02X",
                     mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
            return &s_peers[i];
        }
    }
    return NULL;
}

static void update_peer(const uint8_t *mac, uint32_t height, uint8_t caps) {
    qz_lora_peer_t *peer = find_or_create_peer(mac);
    if (peer) {
        peer->last_seen_sec = esp_timer_get_time() / 1000000;
        peer->known_height = height;
        peer->capabilities = caps;
    }
}

static void prune_stale_peers(void) {
    uint32_t now = esp_timer_get_time() / 1000000;
    uint8_t active = 0;
    for (int i = 0; i < QZ_LORA_MAX_PEERS; i++) {
        if (s_peers[i].active && (now - s_peers[i].last_seen_sec) > QZ_LORA_PEER_TIMEOUT_SEC) {
            ESP_LOGI(TAG, "Peer expired: %02X:%02X:%02X:%02X:%02X:%02X",
                     s_peers[i].mac[0], s_peers[i].mac[1], s_peers[i].mac[2],
                     s_peers[i].mac[3], s_peers[i].mac[4], s_peers[i].mac[5]);
            s_peers[i].active = false;
            memset(s_peers[i].mac, 0, 6);
        }
        if (s_peers[i].active) active++;
    }
    s_stats.active_peers = active;
}

const qz_lora_peer_t *quartz_lora_get_peers(uint8_t *count) {
    prune_stale_peers();
    *count = s_stats.active_peers;
    return s_peers;
}

// ============================================================
// Receive Task
// ============================================================

void quartz_lora_receive_task(void *arg) {
    ESP_LOGI(TAG, "LoRa receive task started");

    uint32_t last_beacon = esp_timer_get_time() / 1000000;

    // Start in continuous RX mode
    sx1276_start_rx(SX1276_RX_CONTINUOUS);

    while (s_lora_initialized) {
        // Periodic beacon
        uint32_t now = esp_timer_get_time() / 1000000;
        if (now - last_beacon >= QZ_LORA_BEACON_INTERVAL_SEC) {
            quartz_lora_beacon();
            last_beacon = now;
            prune_stale_peers();
        }

        // Check for incoming packet
        uint8_t packet[QZ_LORA_TX_BUFFER_SIZE];
        sx1276_packet_info_t info = {0};

        int len = sx1276_check_rx(packet, sizeof(packet), &info);

        if (len > 0) {
            s_stats.packets_received++;
            s_stats.last_rssi = info.rssi;
            s_stats.last_snr = info.snr;

            if (len < QZ_LORA_HEADER_SIZE) {
                // Too short — restart RX
                sx1276_start_rx(SX1276_RX_CONTINUOUS);
                continue;
            }

            qz_lora_header_t *hdr = (qz_lora_header_t *)packet;
            uint8_t *payload = packet + QZ_LORA_HEADER_SIZE;
            size_t payload_len = hdr->payload_len;

            if (payload_len > (size_t)(len - QZ_LORA_HEADER_SIZE)) {
                sx1276_start_rx(SX1276_RX_CONTINUOUS);
                continue;
            }

            // Skip our own packets
            uint8_t my_mac[6] = {0};
            esp_wifi_get_mac(WIFI_IF_STA, my_mac);
            if (memcmp(hdr->sender_id, my_mac, 6) == 0) {
                sx1276_start_rx(SX1276_RX_CONTINUOUS);
                continue;
            }

            // Dedup check
            if (packet_seen(hdr->sender_id, hdr->seq)) {
                ESP_LOGD(TAG, "Duplicate packet ignored (seq=%d)", hdr->seq);
                sx1276_start_rx(SX1276_RX_CONTINUOUS);
                continue;
            }

            ESP_LOGI(TAG, "RX type=%d ttl=%d hops=%d seq=%d RSSI=%d SNR=%d",
                     hdr->type, hdr->ttl, hdr->hop_count, hdr->seq,
                     info.rssi, info.snr);

            // Dispatch by packet type
            switch (hdr->type) {
                case QZ_LORA_PKT_BEACON: {
                    if (payload_len >= sizeof(qz_lora_beacon_t)) {
                        qz_lora_beacon_t *beacon = (qz_lora_beacon_t *)payload;
                        update_peer(hdr->sender_id, beacon->current_height, beacon->capabilities);
                        ESP_LOGI(TAG, "Peer beacon: height=%lu caps=0x%02X",
                                 (unsigned long)beacon->current_height, beacon->capabilities);
                    }
                    break;
                }

                case QZ_LORA_PKT_BLOCK_HDR: {
                    if (payload_len >= sizeof(qz_lora_block_announce_t)) {
                        qz_lora_block_announce_t *block = (qz_lora_block_announce_t *)payload;
                        ESP_LOGI(TAG, "Block announce: height=%lu",
                                 (unsigned long)block->height);

                        uint32_t my_height = quartz_chain_get_height();
                        if (block->height > my_height) {
                            if (s_block_cb) {
                                s_block_cb(block, hdr->sender_id);
                            }
                        }
                    }
                    break;
                }

                case QZ_LORA_PKT_TX: {
                    if (payload_len >= sizeof(qz_lora_tx_relay_t)) {
                        qz_lora_tx_relay_t *tx = (qz_lora_tx_relay_t *)payload;
                        ESP_LOGI(TAG, "TX relay: amount=%llu",
                                 (unsigned long long)tx->amount);
                        if (s_tx_cb) {
                            s_tx_cb(tx, hdr->sender_id);
                        }
                    }
                    break;
                }

                case QZ_LORA_PKT_BLOCK_REQ: {
                    uint32_t requested_height = *(uint32_t *)payload;
                    ESP_LOGI(TAG, "Block request for height %lu",
                             (unsigned long)requested_height);
                    // Search store-and-forward buffer for matching block
                    for (int i = 0; i < QZ_LORA_STORE_FORWARD; i++) {
                        if (s_store_fwd[i].len > QZ_LORA_HEADER_SIZE) {
                            qz_lora_header_t *shdr = (qz_lora_header_t *)s_store_fwd[i].data;
                            if (shdr->type == QZ_LORA_PKT_BLOCK_HDR) {
                                // Resend stored block announcement directly
                                sx1276_transmit(s_store_fwd[i].data, s_store_fwd[i].len);
                                break;
                            }
                        }
                    }
                    break;
                }

                case QZ_LORA_PKT_PING: {
                    // Reply with pong
                    qz_lora_beacon_t pong = {0};
                    pong.current_height = quartz_chain_get_height();
                    quartz_lora_send(QZ_LORA_PKT_PONG, (uint8_t *)&pong, sizeof(pong));
                    break;
                }

                default:
                    ESP_LOGD(TAG, "Unknown packet type: %d", hdr->type);
            }

            // Gossip relay — re-broadcast if TTL > 1
            if (hdr->ttl > 1) {
                hdr->ttl--;
                hdr->hop_count++;

                // Random backoff to avoid collisions (Aloha-style)
                uint32_t backoff_ms = 50 + (esp_random() % 450);
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));

                sx1276_transmit(packet, len);
                s_stats.packets_relayed++;
                ESP_LOGD(TAG, "Relayed packet (ttl=%d)", hdr->ttl);
            }

            // Restart RX
            sx1276_start_rx(SX1276_RX_CONTINUOUS);

        } else if (len == -2) {
            // CRC error — already handled in check_rx
            sx1276_start_rx(SX1276_RX_CONTINUOUS);
        }

        // Brief yield to keep watchdog happy
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "LoRa receive task exiting");
    vTaskDelete(NULL);
}

// ============================================================
// Callbacks
// ============================================================

void quartz_lora_on_block(qz_lora_block_cb cb) { s_block_cb = cb; }
void quartz_lora_on_tx(qz_lora_tx_cb cb) { s_tx_cb = cb; }

// ============================================================
// Stats
// ============================================================

void quartz_lora_get_stats(qz_lora_stats_t *stats) {
    if (stats) memcpy(stats, &s_stats, sizeof(s_stats));
}

// ============================================================
// Store-and-Forward Replay
// ============================================================

int quartz_lora_replay_stored(int max_packets) {
    int replayed = 0;
    uint32_t now = esp_timer_get_time() / 1000000;

    for (int i = 0; i < QZ_LORA_STORE_FORWARD && replayed < max_packets; i++) {
        int idx = (s_store_fwd_idx - 1 - i + QZ_LORA_STORE_FORWARD) % QZ_LORA_STORE_FORWARD;
        if (s_store_fwd[idx].len > 0 && (now - s_store_fwd[idx].timestamp) < 3600) {
            sx1276_transmit(s_store_fwd[idx].data, s_store_fwd[idx].len);
            replayed++;
            vTaskDelay(pdMS_TO_TICKS(100));  // spacing between packets
        }
    }

    if (replayed > 0) {
        ESP_LOGI(TAG, "Replayed %d stored packets", replayed);
    }
    return replayed;
}
