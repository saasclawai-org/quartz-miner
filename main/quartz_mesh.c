/**
 * quartz_mesh.c — ESP-NOW Mesh Networking for Quartz Miners
 *
 * Implementation: broadcast discovery, work relay, block-found relay.
 * Runs on both ESP32 (Heltec V2, M5Stack) and ESP32-C3.
 *
 * License: MIT
 */

#include "quartz_mesh.h"
#include "quartz.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "string.h"

static const char *TAG = "QZ.MESH";

/* ============================================================
 * State
 * ============================================================ */

#define QZ_MESH_CHANNEL    1
#define QZ_MESH_MAX_PEERS  8
#define QZ_MESH_PEER_TIMEOUT_SEC  120   /* drop peer after 2min silence */
#define QZ_MESH_HELLO_INTERVAL_SEC 30  /* re-broadcast HELLO every 30s */

static bool s_initialized = false;
static uint8_t s_mymac[6];
static uint8_t s_caps = 0;
static uint32_t s_mesh_height = 0;   /* v078 */

static qz_mesh_peer_t s_peers[QZ_MESH_MAX_PEERS];
static int s_peer_count = 0;

/* Work relay buffer — latest received work from a mesh peer */
static qz_block_template_t s_rx_work;
static bool s_rx_work_valid = false;

/* Found-block relay buffer — latest received found block from a mesh peer */
static qz_mesh_found_t s_rx_found;
static bool s_rx_found_valid = false;

/* Pending work request from a peer — v076 per-board payouts */
static qz_mesh_work_req_t s_rx_req;
static bool s_rx_req_valid = false;

/* ============================================================
 * Peer management
 * ============================================================ */

static int find_peer(const uint8_t *mac) {
    for (int i = 0; i < s_peer_count; i++) {
        if (memcmp(s_peers[i].mac, mac, 6) == 0) return i;
    }
    return -1;
}

static int add_peer(const uint8_t *mac, uint8_t caps, uint32_t height, uint32_t now) {
    /* Update existing */
    int idx = find_peer(mac);
    if (idx >= 0) {
        s_peers[idx].caps = caps;
        s_peers[idx].last_seen = now;
        if (height > 0) s_peers[idx].height = height;
        return idx;
    }
    /* Add new */
    if (s_peer_count >= QZ_MESH_MAX_PEERS) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < QZ_MESH_MAX_PEERS; i++) {
            if (s_peers[i].last_seen < s_peers[oldest].last_seen) oldest = i;
        }
        idx = oldest;
    } else {
        idx = s_peer_count++;
    }
    memcpy(s_peers[idx].mac, mac, 6);
    s_peers[idx].caps = caps;
    s_peers[idx].last_seen = now;
    s_peers[idx].height = height;

    /* Add ESP-NOW unicast peer */
    esp_now_peer_info_t p = {0};
    p.channel = 0;  /* current channel */
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    memcpy(p.peer_addr, mac, 6);
    esp_now_add_peer(&p);

    ESP_LOGI(TAG, "Peer discovered: %02x%02x%02x%02x%02x%02x (caps=0x%02x height=%u)",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], caps, height);
    return idx;
}

/* ============================================================
 * Send callbacks
 * ============================================================ */

static void on_sent(const uint8_t *mac, esp_now_send_status_t st) {
    /* Silent — too noisy for serial */
    (void)mac; (void)st;
}

/* ============================================================
 * Receive handler
 * ============================================================ */

static void on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
    if (!info || !data || len < 1) return;

    const uint8_t *src = info->src_addr;
    uint32_t now = xTaskGetTickCount() / pdMS_TO_TICKS(1000);

    uint8_t msg_type = data[0];

    switch (msg_type) {
    case QZ_MESH_HELLO: {
        if (len < (int)sizeof(qz_mesh_hello_t)) break;
        const qz_mesh_hello_t *h = (const qz_mesh_hello_t *)data;
        add_peer(src, h->caps, h->height, now);
        break;
    }

    case QZ_MESH_WORK: {
        if (len < (int)sizeof(qz_mesh_work_t)) break;
        /* v076: store for everyone — a board with WiFi but an unreachable
         * node still consumes directed replies. The mining loop only uses
         * mesh work when it has none of its own. */
        const qz_mesh_work_t *w = (const qz_mesh_work_t *)data;
        memcpy(s_rx_work.header, w->header, 80);
        s_rx_work.target_bits = w->target_bits;
        s_rx_work.height = w->height;
        strlcpy(s_rx_work.job_id, w->job_id, sizeof(s_rx_work.job_id));
        s_rx_work_valid = true;
        ESP_LOGI(TAG, "Received work via mesh: block %u, target %u",
                 w->height, w->target_bits);
        break;
    }

    case QZ_MESH_FOUND: {
        if (len < (int)sizeof(qz_mesh_found_t)) break;
        if (!(s_caps & QZ_CAP_HAS_WIFI)) break;  /* only connected peers can submit */
        const qz_mesh_found_t *f = (const qz_mesh_found_t *)data;
        memcpy(s_rx_found.header, f->header, 80);
        s_rx_found.nonce = f->nonce;
        s_rx_found_valid = true;
        ESP_LOGI(TAG, "Block found via mesh peer — relaying to node");
        break;
    }

    case QZ_MESH_WORK_REQ: {
        if (len < (int)sizeof(qz_mesh_work_req_t)) break;
        if (!(s_caps & QZ_CAP_HAS_WIFI)) break;  /* only connected peers can serve */
        memcpy(&s_rx_req, data, sizeof(s_rx_req));
        s_rx_req_valid = true;
        add_peer(src, 0, 0, now);  /* know the requester for the directed reply */
        ESP_LOGI(TAG, "Work request from %02x%02x%02x%02x%02x%02x",
                 src[0], src[1], src[2], src[3], src[4], src[5]);
        break;
    }

    default:
        break;
    }

    /* Track peer last-seen */
    int idx = find_peer(src);
    if (idx >= 0) s_peers[idx].last_seen = now;
}

/* ============================================================
 * Broadcast helpers
 * ============================================================ */

static void broadcast(const uint8_t *data, int len) {
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_send(bcast, data, len);
}

static void send_hello(void) {
    qz_mesh_hello_t hello = {0};
    hello.type = QZ_MESH_HELLO;
    memcpy(hello.mac, s_mymac, 6);
    hello.caps = s_caps;
    hello.height = s_mesh_height;   /* v078: real chain height in beacons */
    broadcast((uint8_t *)&hello, sizeof(hello));
}

/* ============================================================
 * Public API
 * ============================================================ */

int quartz_mesh_init(void) {
    if (s_initialized) return 0;

    /* v089.11: ESP-NOW NULL-derefs into a boot panic if the WiFi driver
     * was never initialized. Reachable in deferred-WiFi boot states
     * (seed unconfirmed, or confirmed with no WiFi creds yet) — mesh must
     * never take the board down; callers retry after WiFi comes up. */
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) != ESP_OK) {
        ESP_LOGW(TAG, "Mesh deferred — WiFi driver not up yet");
        return -1;
    }

    /* ESP-NOW requires WiFi to be started first */
    esp_err_t err = esp_now_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_now_init failed: %s", esp_err_to_name(err));
        return -1;
    }

    esp_now_register_send_cb(on_sent);
    esp_now_register_recv_cb(on_recv);

    /* Add broadcast peer for discovery */
    uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    esp_now_peer_info_t p = {0};
    p.channel = 0;
    p.ifidx = WIFI_IF_STA;
    p.encrypt = false;
    memcpy(p.peer_addr, bcast, 6);
    esp_now_add_peer(&p);

    /* Get our MAC */
    esp_wifi_get_mac(WIFI_IF_STA, s_mymac);

    s_initialized = true;
    s_peer_count = 0;
    s_rx_work_valid = false;
    s_rx_found_valid = false;
    s_rx_req_valid = false;

    uint8_t cur_ch = 0;
    wifi_second_chan_t cur_2nd = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&cur_ch, &cur_2nd);
    ESP_LOGI(TAG, "ESP-NOW mesh initialized (MAC %02x%02x%02x%02x%02x%02x, ch %d)",
             s_mymac[0], s_mymac[1], s_mymac[2], s_mymac[3], s_mymac[4], s_mymac[5],
             cur_ch);

    /* Initial HELLO */
    send_hello();

    return 0;
}

void quartz_mesh_share_work(const qz_block_template_t *tmpl) {
    if (!s_initialized || !tmpl) return;

    qz_mesh_work_t pkt = {0};
    pkt.type = QZ_MESH_WORK;
    memcpy(pkt.header, tmpl->header, 80);
    pkt.target_bits = tmpl->target_bits;
    pkt.height = tmpl->height;
    strlcpy(pkt.job_id, tmpl->job_id, sizeof(pkt.job_id));

    broadcast((uint8_t *)&pkt, sizeof(pkt));
}

void quartz_mesh_share_found(const uint8_t header[80], uint64_t nonce) {
    if (!s_initialized) return;

    qz_mesh_found_t pkt = {0};
    pkt.type = QZ_MESH_FOUND;
    memcpy(pkt.header, header, 80);
    pkt.nonce = nonce;

    broadcast((uint8_t *)&pkt, sizeof(pkt));
    ESP_LOGI(TAG, "Broadcasted block found to mesh peers");
}

void quartz_mesh_request_work(const char *address) {
    if (!s_initialized || !address) return;

    qz_mesh_work_req_t pkt = {0};
    pkt.type = QZ_MESH_WORK_REQ;
    memcpy(pkt.mac, s_mymac, 6);
    strlcpy(pkt.address, address, sizeof(pkt.address));
    broadcast((uint8_t *)&pkt, sizeof(pkt));
}

int quartz_mesh_get_work_req(char address[40], uint8_t mac[6]) {
    if (!s_initialized || !s_rx_req_valid || !address || !mac) return -1;

    strlcpy(address, s_rx_req.address, 40);
    memcpy(mac, s_rx_req.mac, 6);
    s_rx_req_valid = false;  /* consume */
    return 0;
}

void quartz_mesh_send_work_to(const uint8_t mac[6], const qz_block_template_t *tmpl) {
    if (!s_initialized || !mac || !tmpl) return;

    qz_mesh_work_t pkt = {0};
    pkt.type = QZ_MESH_WORK;
    memcpy(pkt.header, tmpl->header, 80);
    pkt.target_bits = tmpl->target_bits;
    pkt.height = tmpl->height;
    strlcpy(pkt.job_id, tmpl->job_id, sizeof(pkt.job_id));
    esp_now_send(mac, (uint8_t *)&pkt, sizeof(pkt));
}

int quartz_mesh_get_work(qz_block_template_t *tmpl) {
    if (!s_initialized || !s_rx_work_valid || !tmpl) return -1;

    memcpy(tmpl, &s_rx_work, sizeof(*tmpl));
    s_rx_work_valid = false;  /* consume */
    return 0;
}

int quartz_mesh_get_found(uint8_t header[80], uint64_t *nonce) {
    if (!s_initialized || !s_rx_found_valid || !header || !nonce) return -1;

    memcpy(header, s_rx_found.header, 80);
    *nonce = s_rx_found.nonce;
    s_rx_found_valid = false;  /* consume */
    return 0;
}

void quartz_mesh_update_caps(uint8_t caps) {
    s_caps = caps;
    if (s_initialized) send_hello();
}

void quartz_mesh_update_height(uint32_t height) {
    s_mesh_height = height;
}

int quartz_mesh_get_peers(qz_mesh_peer_t *peers, int max_count) {
    if (!s_initialized || !peers) return 0;
    int n = (s_peer_count < max_count) ? s_peer_count : max_count;
    memcpy(peers, s_peers, n * sizeof(qz_mesh_peer_t));
    return n;
}

bool quartz_mesh_is_active(void) {
    return s_initialized;
}

void quartz_mesh_step(uint32_t uptime_sec) {
    if (!s_initialized) return;

    /* v078: single time base — last_seen is stamped from the tick counter
     * in on_recv; mixing it with the caller's esp_timer-derived uptime
     * caused spurious peer timeouts. One clock for both. */
    uptime_sec = xTaskGetTickCount() / pdMS_TO_TICKS(1000);

    /* Prune stale peers */
    bool pruned = false;
    for (int i = 0; i < s_peer_count; ) {
        if (uptime_sec - s_peers[i].last_seen > QZ_MESH_PEER_TIMEOUT_SEC) {
            ESP_LOGI(TAG, "Peer timed out: %02x%02x%02x%02x%02x%02x",
                     s_peers[i].mac[0], s_peers[i].mac[1], s_peers[i].mac[2],
                     s_peers[i].mac[3], s_peers[i].mac[4], s_peers[i].mac[5]);
            /* Remove ESP-NOW peer */
            esp_now_del_peer(s_peers[i].mac);
            /* Shift down */
            if (i < s_peer_count - 1) {
                s_peers[i] = s_peers[s_peer_count - 1];
            }
            s_peer_count--;
            pruned = true;
        } else {
            i++;
        }
    }

    /* Re-broadcast HELLO periodically */
    static uint32_t last_hello = 0;
    if (uptime_sec - last_hello >= QZ_MESH_HELLO_INTERVAL_SEC) {
        last_hello = uptime_sec;
        send_hello();
    }
}
