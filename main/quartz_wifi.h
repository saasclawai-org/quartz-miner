/**
 * quartz_wifi.h — WiFi + Mining Protocol Client for Quartz ESP32
 *
 * Provides:
 * - WiFi provisioning via captive portal (first boot)
 * - NVS credential storage
 * - TCP connection to Quartz reference node
 * - Mining protocol: get work, submit shares
 */

#ifndef QUARTZ_WIFI_H
#define QUARTZ_WIFI_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi states */
typedef enum {
    QZ_WIFI_UNPROVISIONED = 0,
    QZ_WIFI_CONNECTING,
    QZ_WIFI_CONNECTED,
    QZ_WIFI_PORTAL_ACTIVE,
    QZ_WIFI_ERROR,
} qz_wifi_state_t;

/* Mining protocol states */
typedef enum {
    QZ_MINING_IDLE = 0,
    QZ_MINING_FETCHING_WORK,
    QZ_MINING_HAS_WORK,
    QZ_MINING_SUBMITTING,
    QZ_MINING_SUBMITTED,
} qz_mining_state_t;

/* Block template from node */
typedef struct {
    uint8_t  header[80];       /* Block header to mine on */
    uint32_t target_bits;      /* Difficulty target */
    uint32_t height;           /* Block height */
    char     job_id[32];       /* Job ID from server */
} qz_block_template_t;

/* Current state */
extern qz_wifi_state_t g_wifi_state;
extern qz_mining_state_t g_mining_state;

/* === Lifecycle === */

/**
 * Initialize WiFi subsystem.
 * If credentials exist in NVS → connect to AP.
 * If not → start captive portal for provisioning.
 */
void quartz_wifi_init(void);
void quartz_wifi_set_full_power(void);

/**
 * Check if WiFi is connected.
 */
bool quartz_wifi_is_connected(void);

/**
 * Wait for WiFi connection (blocking, up to timeout_ms).
 */
bool quartz_wifi_wait_connected(int timeout_ms);

/**
 * Get current IP address string (or NULL if not connected).
 */
const char *quartz_wifi_get_ip(void);

/* === Mining Protocol === */

/**
 * Fetch block template from reference node.
 * Returns QZ_OK on success, template filled in.
 */
int quartz_mining_get_work(qz_block_template_t *tmpl);

/**
 * Submit a found block (nonce that meets target).
 * Returns QZ_OK if accepted by node.
 */
int quartz_mining_submit(const char *job_id, uint64_t nonce, const uint8_t header[80]);

/**
 * Check if the node is reachable.
 */
bool quartz_mining_node_reachable(void);

/* === Messaging === */

/* Latest message from the chain */
typedef struct {
    char from[32];
    char text[160];
    int  block_height;
    bool confirmed;
} qz_message_t;

/* Fetch latest message from node. Returns 0 on success. */
int quartz_messages_get_latest(qz_message_t *msg);

/* Send a message via the chain. Returns 0 on success. */
int quartz_messages_send(const char *from, const char *to, const char *text);

/**
 * Public HTTP request to the Quartz node.
 * Returns response body length, or negative on error.
 */
int quartz_http_request(const char *method, const char *path,
                         const char *body, char *response, size_t resp_size);

/* === Captive Portal Seed Provisioning === */

/**
 * Set seed phrase for captive portal /seed endpoint.
 * Called by main.c after wallet creation.
 */
void quartz_wifi_portal_set_seed(const char words[12][12], const char *address);

/**
 * Check if seed was confirmed via captive portal.
 */
bool quartz_wifi_portal_seed_confirmed(void);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_WIFI_H
