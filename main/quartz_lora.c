/**
 * Quartz LoRa Mesh Implementation — SX1262 (Heltec WiFi LoRa 32 V3)
 *
 * Uses RadioLib for SX1262 driver. Also compatible with SX1276 (TTGO LoRa32).
 *
 * Pin configuration (Heltec WiFi LoRa 32 V3):
 *   SCK    = GPIO 9
 *   MISO   = GPIO 11
 *   MOSI   = GPIO 10
 *   CS     = GPIO 8
 *   RST    = GPIO 12
 *   DIO1   = GPIO 14
 *   BUSY   = GPIO 13
 *
 * Pin configuration (TTGO LoRa32 V2.1, SX1276):
 *   SCK    = GPIO 5
 *   MISO   = GPIO 19
 *   MOSI   = GPIO 27
 *   CS     = GPIO 18
 *   RST    = GPIO 14
 *   DIO0   = GPIO 26
 */

#include "quartz_lora.h"
#include "quartz_wallet.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>

static const char *TAG = "QUARTZ_LORA";

// RadioLib — installed via idf_component.yml
//#include <RadioLib.h>

// SX1262 (Heltec V3)
SPIClass spi(FSPI);
SX1262 radio = new Module(8, 14, 12, 13, spi);  // CS, DIO1, RST, BUSY

// State
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

// ============================================================
// Init
// ============================================================

int quartz_lora_init(uint32_t freq_hz) {
    s_current_freq = freq_hz;

    // SPI init for SX1262
    spi.begin(9, 11, 10, 8);  // SCK, MISO, MOSI, CS

    int state = radio.begin(freq_hz, QZ_LORA_BANDWIDTH, QZ_LORA_SPREADING_FACTOR,
                            QZ_LORA_CODING_RATE, QZ_LORA_SYNC_WORD,
                            QZ_LORA_TX_POWER, QZ_LORA_PREAMBLE_LEN, 0);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGE(TAG, "LoRa init failed: %d", state);
        return -1;
    }

    // Enable CRC
    radio.setCRC(true);

    // Enable implicit header mode for fixed-size packets? No — use explicit.
    // LoRa uses explicit headers by default in RadioLib.

    s_lora_initialized = true;
    memset(s_peers, 0, sizeof(s_peers));

    ESP_LOGI(TAG, "LoRa initialized: %lu Hz, SF%d, BW %d kHz, TX %d dBm",
             (unsigned long)freq_hz, QZ_LORA_SPREADING_FACTOR,
             QZ_LORA_BANDWIDTH / 1000, QZ_LORA_TX_POWER);
    ESP_LOGI(TAG, "Expected range: 2-5km urban, 10-15km line-of-sight");

    return 0;
}

void quartz_lora_set_region(uint32_t freq_hz) {
    s_current_freq = freq_hz;
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

    // Get our MAC for sender_id
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    memcpy(hdr->sender_id, mac, 6);

    // Copy payload
    memcpy(packet + QZ_LORA_HEADER_SIZE, payload, len);
    size_t total_len = QZ_LORA_HEADER_SIZE + len;

    // Send
    int state = radio.transmit(packet, total_len);
    if (state != RADIOLIB_ERR_NONE) {
        ESP_LOGW(TAG, "LoRa TX failed: %d", state);
        return -3;
    }

    s_stats.packets_sent++;

    // Store for late peer replay
    memcpy(s_store_fwd[s_store_fwd_idx].data, packet, total_len);
    s_store_fwd[s_store_fwd_idx].len = total_len;
    s_store_fwd[s_store_fwd_idx].timestamp = esp_timer_get_time() / 1000000;
    s_store_fwd_idx = (s_store_fwd_idx + 1) % QZ_LORA_STORE_FORWARD;

    ESP_LOGD(TAG, "TX type=%d len=%d", type, total_len);
    return 0;
}

// ============================================================
// Beacon
// ============================================================

int quartz_lora_beacon(void) {
    qz_lora_beacon_t beacon = {0};
    beacon.current_height = quartz_chain_get_height();
    // beacon.current_hash = first 8 bytes of best hash
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
    // Find existing
    for (int i = 0; i < QZ_LORA_MAX_PEERS; i++) {
        if (s_peers[i].active && memcmp(s_peers[i].mac, mac, 6) == 0) {
            return &s_peers[i];
        }
    }
    // Find empty slot
    for (int i = 0; i < QZ_LORA_MAX_PEERS; i++) {
        if (!s_peers[i].active) {
            memcpy(s_peers[i].mac, mac, 6);
            s_peers[i].active = true;
            return &s_peers[i];
        }
    }
    return NULL; // peer table full
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

    uint32_t last_beacon = 0;

    while (s_lora_initialized) {
        // Periodic beacon
        uint32_t now = esp_timer_get_time() / 1000000;
        if (now - last_beacon >= QZ_LORA_BEACON_INTERVAL_SEC) {
            quartz_lora_beacon();
            last_beacon = now;
            prune_stale_peers();
        }

        // Check for incoming packet
        int state = radio.receive(1000);  // 1 second timeout

        if (state == RADIOLIB_ERR_NONE) {
            // Got a packet
            uint8_t packet[QZ_LORA_TX_BUFFER_SIZE];
            size_t len = radio.getPacketLength();
            if (len > QZ_LORA_TX_BUFFER_SIZE) len = QZ_LORA_TX_BUFFER_SIZE;
            radio.readData(packet, len);

            s_stats.packets_received++;
            s_stats.last_rssi = radio.getRSSI();
            s_stats.last_snr = radio.getSNR();

            if (len < QZ_LORA_HEADER_SIZE) continue;

            qz_lora_header_t *hdr = (qz_lora_header_t *)packet;
            uint8_t *payload = packet + QZ_LORA_HEADER_SIZE;
            size_t payload_len = hdr->payload_len;

            if (payload_len > (len - QZ_LORA_HEADER_SIZE)) continue;

            // Skip our own packets
            uint8_t my_mac[6];
            esp_wifi_get_mac(WIFI_IF_STA, my_mac);
            if (memcmp(hdr->sender_id, my_mac, 6) == 0) continue;

            ESP_LOGD(TAG, "RX type=%d ttl=%d hops=%d from %02X:%02X:%02X:%02X:%02X:%02X RSSI=%d SNR=%.1f",
                     hdr->type, hdr->ttl, hdr->hop_count,
                     hdr->sender_id[0], hdr->sender_id[1], hdr->sender_id[2],
                     hdr->sender_id[3], hdr->sender_id[4], hdr->sender_id[5],
                     s_stats.last_rssi, s_stats.last_snr);

            // Dispatch by packet type
            switch (hdr->type) {
                case QZ_LORA_PKT_BEACON: {
                    qz_lora_beacon_t *beacon = (qz_lora_beacon_t *)payload;
                    update_peer(hdr->sender_id, beacon->current_height, beacon->capabilities);
                    ESP_LOGI(TAG, "Peer beacon: height=%lu caps=0x%02X",
                             (unsigned long)beacon->current_height, beacon->capabilities);
                    break;
                }

                case QZ_LORA_PKT_BLOCK_HDR: {
                    qz_lora_block_announce_t *block = (qz_lora_block_announce_t *)payload;
                    ESP_LOGI(TAG, "Block announce: height=%lu",
                             (unsigned long)block->height);

                    // Check if we need this block
                    uint32_t my_height = quartz_chain_get_height();
                    if (block->height > my_height) {
                        // We're behind — try to get full block via WiFi
                        // or request via LoRa store-and-forward
                        if (s_block_cb) {
                            s_block_cb(block, hdr->sender_id);
                        }
                    }
                    break;
                }

                case QZ_LORA_PKT_TX: {
                    qz_lora_tx_relay_t *tx = (qz_lora_tx_relay_t *)payload;
                    ESP_LOGI(TAG, "TX relay: amount=%llu sats",
                             (unsigned long long)tx->amount);

                    // Add to mempool if valid
                    if (s_tx_cb) {
                        s_tx_cb(tx, hdr->sender_id);
                    }
                    break;
                }

                case QZ_LORA_PKT_BLOCK_REQ: {
                    // Peer needs a block — check store-and-forward
                    uint32_t requested_height = *(uint32_t *)payload;
                    ESP_LOGI(TAG, "Block request for height %lu",
                             (unsigned long)requested_height);

                    // Search store-and-forward buffer
                    for (int i = 0; i < QZ_LORA_STORE_FORWARD; i++) {
                        if (s_store_fwd[i].len > 0) {
                            // Resend stored block announcement
                            radio.transmit(s_store_fwd[i].data, s_store_fwd[i].len);
                        }
                    }
                    break;
                }

                default:
                    ESP_LOGD(TAG, "Unknown packet type: %d", hdr->type);
            }

            // Gossip relay — re-broadcast if TTL > 1
            if (hdr->ttl > 1) {
                hdr->ttl--;
                hdr->hop_count++;

                // Small random delay to avoid collisions (Aloha-style)
                vTaskDelay(pdMS_TO_TICKS(100 + (esp_random() % 500)));

                radio.transmit(packet, len);
                s_stats.packets_relayed++;

                ESP_LOGD(TAG, "Relayed packet (ttl=%d)", hdr->ttl);
            }

        } else if (state != RADIOLIB_ERR_RX_TIMEOUT) {
            ESP_LOGW(TAG, "LoRa RX error: %d", state);
        }
    }
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
