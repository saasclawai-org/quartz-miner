/**
 * quartz_wifi.c — WiFi Provisioning + Mining Protocol Client
 *
 * First boot: Starts captive portal AP "Quartz-XXXX"
 *   Phone connects → browser opens → enter WiFi password
 *   Credentials saved to NVS
 * Subsequent boots: Auto-connects to saved AP
 *
 * Mining: HTTP client fetches work from reference node
 *   GET  /api/v1/mining/work → block template
 *   POST /api/v1/mining/submit → submit found nonce
 */

#include "quartz_wifi.h"
#include "quartz.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "esp_timer.h"
#include "esp_mac.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#define vTaskDelay(x)
#endif

static const char *TAG = "QZ.WIFI";

/* Seed phrase for portal display (set by main.c after wallet creation) */
static char s_portal_seed[12][12] = {0};
static char s_portal_address[64] = {0};
static bool s_portal_seed_available = false;
static bool s_portal_seed_confirmed = false;
static int s_portal_challenge_idx = 0;  /* random word index for confirmation */

/* Node configuration */
/* Node port — override at build time (-DNODE_PORT=21100) or edit here.
 * LAN gateways (Pi bundle) listen on 21100; the seed's port 80 is nginx. */
#ifndef NODE_PORT
#define NODE_PORT   80
#endif
#define NODE_PATH_WORK    "/api/v1/mining/work"
#define NODE_PATH_SUBMIT  "/api/v1/mining/submit"
#define NODE_PATH_INFO    "/api/v1/info"

/* NVS keys */
#define WIFI_NVS_NS   "qz_wifi"
#define WIFI_KEY_SSID "ssid"
#define WIFI_KEY_PASS "pass"
#define WIFI_KEY_NODE "node"

/* State */
qz_wifi_state_t g_wifi_state = QZ_WIFI_UNPROVISIONED;
qz_mining_state_t g_mining_state = QZ_MINING_IDLE;

static char s_ip_str[16] = {0};
static bool s_got_ip = false;
static bool s_wifi_started = false;

/* Runtime node endpoint — set from NVS (captive portal) at init,
 * falls back to the compile-time NODE_HOST/NODE_PORT defaults. */
static char s_node_host[64] = {0};
static int  s_node_port = -1;   /* -1 = not loaded yet */

const char *quartz_wifi_node_host(void) {
    return s_node_host[0] ? s_node_host : NODE_HOST;
}

int quartz_wifi_node_port(void) {
    return s_node_port > 0 ? s_node_port : NODE_PORT;
}

#ifdef ESP_PLATFORM

/* ============================================================
 * NVS credential storage
 * ============================================================ */

static bool load_wifi_creds(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;

    esp_err_t e1 = nvs_get_str(h, WIFI_KEY_SSID, ssid, &ssid_len);
    esp_err_t e2 = nvs_get_str(h, WIFI_KEY_PASS, pass, &pass_len);
    nvs_close(h);

    return (e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0');
}

static void save_wifi_creds(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, WIFI_KEY_SSID, ssid);
    nvs_set_str(h, WIFI_KEY_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* v089.10: does the board already know its WiFi? Setup isn't done
 * (BLE stays available) until it does. */
bool quartz_wifi_has_creds(void) {
    char ssid[33], pass[65];
    return load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));
}

/* v089.9: BLE provisioning — SSID and password arrive as two separate
 * encrypted writes; stage each and commit the pair to NVS once both
 * are present (the app sends SSID first, then the password). */
void quartz_wifi_set_credentials(const char *ssid, const char *pass) {
    static char staged_ssid[33] = {0};
    static char staged_pass[65] = {0};
    if (ssid) snprintf(staged_ssid, sizeof(staged_ssid), "%s", ssid);
    if (pass) snprintf(staged_pass, sizeof(staged_pass), "%s", pass);
    if (staged_ssid[0] != '\0' && staged_pass[0] != '\0') {
        save_wifi_creds(staged_ssid, staged_pass);
        ESP_LOGI(TAG, "📡 WiFi credentials received over BLE — reboot to join");
    }
}

/* --- Node endpoint config (host[:port]) --------------------------------- */

/* Parse "host[:port]" into the runtime endpoint. Port defaults to 80
 * (nginx-fronted nodes). Returns false on invalid spec (state unchanged). */
static bool node_parse(const char *spec) {
    char host[64] = {0};
    int port = 80;
    const char *colon = strchr(spec, ':');
    if (colon) {
        size_t hlen = (size_t)(colon - spec);
        if (hlen == 0 || hlen >= sizeof(host)) return false;
        memcpy(host, spec, hlen);
        port = atoi(colon + 1);
        if (port < 1 || port > 65535) return false;
    } else {
        if (!spec[0] || strlen(spec) >= sizeof(host)) return false;
        memcpy(host, spec, strlen(spec));
    }
    snprintf(s_node_host, sizeof(s_node_host), "%s", host);
    s_node_port = port;
    return true;
}

static void load_node_config(void) {
    char spec[80] = {0};
    size_t len = sizeof(spec);
    /* Compile-time defaults first */
    snprintf(s_node_host, sizeof(s_node_host), "%s", NODE_HOST);
    s_node_port = NODE_PORT;

    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    if (nvs_get_str(h, WIFI_KEY_NODE, spec, &len) == ESP_OK && spec[0]) {
        if (node_parse(spec)) {
            ESP_LOGI(TAG, "🎯 Node endpoint (portal): %s:%d", s_node_host, s_node_port);
        } else {
            ESP_LOGW(TAG, "bad node spec '%s' in NVS — using default", spec);
        }
    }
    nvs_close(h);
}

static void save_node_spec(const char *spec) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, WIFI_KEY_NODE, spec);
    nvs_commit(h);
    nvs_close(h);
}

static void clear_node_spec(void) {
    nvs_handle_t h;
    if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, WIFI_KEY_NODE);
    nvs_commit(h);
    nvs_close(h);
}

/* ============================================================
 * Event handler
 * ============================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                                int32_t id, void *data) {
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            ESP_LOGI(TAG, "STA started, connecting...");
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "Disconnected (reason %d), retrying...", d->reason);
            s_got_ip = false;
            g_wifi_state = QZ_WIFI_CONNECTING;
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *e = (wifi_event_ap_staconnected_t *)data;
            ESP_LOGI(TAG, "Phone connected to portal (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
                     e->mac[0],e->mac[1],e->mac[2],e->mac[3],e->mac[4],e->mac[5]);
            break;
        }
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&e->ip_info.ip));
        s_got_ip = true;
        g_wifi_state = QZ_WIFI_CONNECTED;
        ESP_LOGI(TAG, "✅ WiFi connected! IP: %s", s_ip_str);
    }
}

/* ============================================================
 * Captive Portal (simple HTTP server on SoftAP)
 * ============================================================ */

/* Minimal captive portal: serves a form at 192.168.4.1
 * User enters SSID + password, we save and reboot.
 *
 * We use raw lwip sockets instead of esp_http_server to keep
 * the binary size down and avoid extra component deps.
 */

/* Format string: %s = current node endpoint (host or host:port) */
static const char *PORTAL_HTML_FMT =
    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
    "<!DOCTYPE html><html><head>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>🔮 Quartz Miner Setup</title>"
    "<style>"
    "body{font-family:system-ui;max-width:400px;margin:40px auto;padding:20px;"
    "background:#1a1a2e;color:#eee;text-align:center}"
    "h1{color:#9933ff}input{width:100%;padding:14px;margin:8px 0;"
    "border:1px solid #444;border-radius:8px;background:#2a2a4e;color:#fff;"
    "box-sizing:border-box;font-size:16px}"
    "button{width:100%;padding:14px;border:none;border-radius:8px;"
    "background:#9933ff;color:#fff;font-size:18px;cursor:pointer;margin-top:8px}"
    "p{color:#888;font-size:14px}"
    "</style></head><body>"
    "<h1>🔮 Quartz</h1>"
    "<p>Connect your miner to WiFi</p>"
    "<form action='/save' method='POST'>"
    "<input name='ssid' placeholder='WiFi Name (SSID)' required>"
    "<input name='pass' type='password' placeholder='WiFi Password'>"
    "<input name='node' placeholder='Node (host[:port])' value='%s'>"
    "<p>Node = where this miner gets work &amp; submits shares.<br>"
    "Point it at your own Raspberry Pi gateway (e.g. 192.168.1.142:21100)"
    " or leave the default seed.</p>"
    "<button type='submit'>Start Mining ⚡</button>"
    "</form>"
    "<p>Miner will restart and begin mining automatically</p>"
    "</body></html>";

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* URL-encoded form field extraction. Field-boundary aware: matches the
 * name only at the start of the body or right after '&', so an SSID
 * containing "pass=" can't be mistaken for the password field. Decodes
 * '+' (space) and %XX. Returns decoded length, 0 if absent. */
static size_t form_field(const char *body, const char *name,
                         char *out, size_t out_len) {
    size_t nlen = strlen(name);
    if (out_len < 2) return 0;
    const char *p = body;
    while (p && *p) {
        if (((p == body) || (p > body && p[-1] == '&')) &&
            strncmp(p, name, nlen) == 0 && p[nlen] == '=') {
            const char *v = p + nlen + 1;
            size_t oi = 0;
            while (*v && *v != '&' && oi + 1 < out_len) {
                if (*v == '+') { out[oi++] = ' '; v++; }
                else if (v[0] == '%' && v[1] && v[2]) {
                    int hi = hexval(v[1]), lo = hexval(v[2]);
                    if (hi >= 0 && lo >= 0) { out[oi++] = (char)((hi << 4) | lo); v += 3; }
                    else out[oi++] = *v++;
                } else out[oi++] = *v++;
            }
            out[oi] = '\0';
            return oi;
        }
        p = strchr(p, '&');
        if (p) p++;
    }
    out[0] = '\0';
    return 0;
}

static void portal_task(void *pv) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { vTaskDelete(NULL); return; }

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "Portal bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    listen(sock, 1);
    ESP_LOGI(TAG, "📱 Captive portal running on 192.168.4.1");

    while (1) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int csock = accept(sock, (struct sockaddr *)&client, &clen);
        if (csock < 0) continue;

        char buf[1024] = {0};
        int len = recv(csock, buf, sizeof(buf) - 1, 0);
        if (len <= 0) { close(csock); continue; }

        /* Check if this is a form submission */
        if (strstr(buf, "POST /save") != NULL) {
            /* Parse SSID and password from body */
            char *body = strstr(buf, "\r\n\r\n");
            if (body) body += 4;

            char ssid[64] = {0}, pass[64] = {0}, node[80] = {0};
            if (body) {
                form_field(body, "ssid", ssid, sizeof(ssid));
                form_field(body, "pass", pass, sizeof(pass));
                form_field(body, "node", node, sizeof(node));
            }

            if (ssid[0]) {
                save_wifi_creds(ssid, pass);
                if (node[0]) {
                    if (node_parse(node)) {
                        save_node_spec(node);
                        ESP_LOGI(TAG, "✅ Node endpoint saved: %s:%d",
                                 s_node_host, s_node_port);
                    } else {
                        ESP_LOGW(TAG, "⚠️ invalid node '%s' ignored (keep existing)", node);
                    }
                } else {
                    clear_node_spec();   /* empty field = compiled default */
                }
                ESP_LOGI(TAG, "✅ WiFi creds saved: SSID='%s'", ssid);

                const char *resp =
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n"
                    "<!DOCTYPE html><html><head>"
                    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                    "<title>Quartz Setup Complete</title>"
                    "<style>body{font-family:system-ui;text-align:center;padding:40px;"
                    "background:#1a1a2e;color:#eee}h1{color:#9933ff}</style>"
                    "</head><body><h1>✅ Saved!</h1>"
                    "<p>Quartz miner is restarting and connecting to WiFi...</p>"
                    "<p>Mining will start automatically!</p>"
                    "</body></html>";
                send(csock, resp, strlen(resp), 0);
                close(csock);

                vTaskDelay(pdMS_TO_TICKS(2000));
                ESP_LOGI(TAG, "Rebooting to apply WiFi config...");
                esp_restart();
                return;
            }
        }

        /* NOTE: Seed phrase is NO LONGER served over WiFi/captive portal.
         * Seed is only available via:
         *   1. Serial QR code (scan with phone app)
         *   2. Device display QR code (M5Stack)
         *   3. BLE (bonded devices only)
         * This is a security hardening — WiFi is plaintext, anyone nearby can sniff. */

        /* Serve the portal page (with current node endpoint prefilled).
         * page is static — portal task stack is 4K and already holds buf. */
        static char page[2560];
        char nodeval[80];
        if (quartz_wifi_node_port() == 80)
            snprintf(nodeval, sizeof(nodeval), "%s", quartz_wifi_node_host());
        else
            snprintf(nodeval, sizeof(nodeval), "%s:%d",
                     quartz_wifi_node_host(), quartz_wifi_node_port());
        int plen = snprintf(page, sizeof(page), PORTAL_HTML_FMT, nodeval);
        if (plen < 0) plen = 0;
        if ((size_t)plen >= sizeof(page)) plen = (int)sizeof(page) - 1;
        send(csock, page, plen, 0);
        close(csock);
    }
}

/* ============================================================
 * WiFi Start (captive portal or station mode)
 * ============================================================ */

static void start_captive_portal(void) {
    ESP_LOGI(TAG, "📱 Starting captive portal (no WiFi creds in NVS)");

    /* Generate AP name from MAC */
    uint8_t mac[6];
    esp_wifi_get_mac(WIFI_IF_STA, mac);
    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "Quartz-%02X%02X", mac[4], mac[5]);

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 2;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    g_wifi_state = QZ_WIFI_PORTAL_ACTIVE;

    /* Start portal HTTP server task */
    xTaskCreate(portal_task, "portal", 4096, NULL, 5, NULL);
}

static void start_sta_mode(const char *ssid, const char *pass) {
    ESP_LOGI(TAG, "📡 Connecting to WiFi: %s", ssid);

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, pass, sizeof(sta_config.sta.password));

    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    esp_wifi_start();

    g_wifi_state = QZ_WIFI_CONNECTING;
}

void quartz_wifi_set_coex_power(void) {
    /* v083: BLE pair window open — MIN_MODEM coexists with BLE advertising */
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
    ESP_LOGI(TAG, "WiFi coex mode (MIN_MODEM) — BLE active");
}

void quartz_wifi_set_full_power(void) {
    /* PS_NONE keeps the radio always-on (v070): no missed ACKs, no
     * reason-34 evictions, no reconnect churn. Safe only when BLE is off. */
    esp_wifi_set_ps(WIFI_PS_NONE);
    ESP_LOGI(TAG, "WiFi full power (PS_NONE) — radio dedicated to WiFi");
}

void quartz_wifi_init(void) {
    /* Initialize TCP/IP stack and event loop (must happen before any WiFi calls) */
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    esp_netif_create_default_wifi_ap();

    /* WiFi init config */
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    /* Register event handlers */
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);

    /* Runtime node endpoint: NVS override (portal) or compile-time default */
    load_node_config();

    char ssid[64] = {0}, pass[64] = {0};

    if (load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
        start_sta_mode(ssid, pass);
    } else {
        start_captive_portal();
    }
    s_wifi_started = true;
}

bool quartz_wifi_is_connected(void) {
    return s_got_ip && (g_wifi_state == QZ_WIFI_CONNECTED);
}

bool quartz_wifi_wait_connected(int timeout_ms) {
    int waited = 0;
    while (!s_got_ip && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(500));
        waited += 500;
    }
    return s_got_ip;
}

const char *quartz_wifi_get_ip(void) {
    return s_got_ip ? s_ip_str : NULL;
}

/* ============================================================
 * Mining Protocol — HTTP client to reference node
 * ============================================================ */

/* Simple HTTP GET/POST using raw sockets (avoids esp_http_client deps) */

static int http_request(const char *method, const char *path,
                         const char *body, char *response, size_t resp_size) {
    if (!s_got_ip) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* Connect to node (DNS resolve first) */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)s_node_port);

    /* DNS resolution */
    struct hostent *he = gethostbyname(s_node_host);
    if (!he || !he->h_addr_list[0]) {
        ESP_LOGW(TAG, "DNS failed for %s", s_node_host);
        close(sock);
        return -3;
    }
    struct in_addr **addr_list = (struct in_addr **)he->h_addr_list;
    addr.sin_addr = *addr_list[0];
    ESP_LOGD(TAG, "DNS: %s -> %s", s_node_host, inet_ntoa(addr.sin_addr));

    /* Set timeout */
    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -2;
    }

    /* Send request */
    char req[1024];
    int req_len;
    if (body) {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n%s",
            method, path, s_node_host, (int)strlen(body), body);
    } else {
        req_len = snprintf(req, sizeof(req),
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, s_node_host);
    }
    send(sock, req, req_len, 0);

    /* Read response */
    int total = 0;
    while (total < (int)resp_size - 1) {
        int n = recv(sock, response + total, resp_size - total - 1, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = '\0';
    close(sock);

    /* Find body (after \r\n\r\n) */
    char *resp_body = strstr(response, "\r\n\r\n");
    if (resp_body) {
        resp_body += 4;
        /* Move body to start of buffer */
        int body_len = total - (resp_body - response);
        memmove(response, resp_body, body_len);
        response[body_len] = '\0';
        return body_len;
    }

    return total;
}

/* Public wrapper for HTTP requests (used by agent module) */
int quartz_http_request(const char *method, const char *path,
                         const char *body, char *response, size_t resp_size) {
    return http_request(method, path, body, response, resp_size);
}

/* Simple JSON value extraction (finds "key":"value" or "key":number) */
static bool json_find_string(const char *json, const char *key, char *out, size_t out_len) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    /* Skip : and whitespace */
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (*p == '"') {
        p++;
        char *end = strchr(p, '"');
        if (!end) return false;
        size_t len = end - p;
        if (len >= out_len) len = out_len - 1;
        memcpy(out, p, len);
        out[len] = '\0';
        return true;
    }
    return false;
}

static bool json_find_int(const char *json, const char *key, int *out) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    char *p = strstr(json, pattern);
    if (!p) return false;
    p += strlen(pattern);
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return true;
}

static bool json_find_hex(const char *json, const char *key, uint8_t *out, size_t out_len) {
    char hexstr[256];
    if (!json_find_string(json, key, hexstr, sizeof(hexstr))) return false;
    size_t hlen = strlen(hexstr);
    if (hlen / 2 > out_len) return false;
    for (size_t i = 0; i < hlen / 2; i++) {
        char hi = hexstr[i * 2], lo = hexstr[i * 2 + 1];
        uint8_t byte = 0;
        if (hi >= '0' && hi <= '9') byte = (hi - '0') << 4;
        else if (hi >= 'a' && hi <= 'f') byte = (hi - 'a' + 10) << 4;
        else if (hi >= 'A' && hi <= 'F') byte = (hi - 'A' + 10) << 4;
        if (lo >= '0' && lo <= '9') byte |= (lo - '0');
        else if (lo >= 'a' && lo <= 'f') byte |= (lo - 'a' + 10);
        else if (lo >= 'A' && lo <= 'F') byte |= (lo - 'A' + 10);
        out[i] = byte;
    }
    return true;
}

int quartz_mining_get_work_for(const char *address, qz_block_template_t *tmpl) {
    if (!quartz_wifi_is_connected() || !address || !address[0]) return -1;

    /* v076: fetch a template paying the GIVEN address (ours, or a mesh
     * peer's that requested work). Hashrate reporting stays ours. */
    extern uint32_t g_last_hps;
    char path[256];
    snprintf(path, sizeof(path), "%s?address=%s&hashrate=%lu",
             NODE_PATH_WORK, address, (unsigned long)g_last_hps);

    char response[2048];
    int n = http_request("GET", path, NULL, response, sizeof(response));
    if (n <= 0) {
        ESP_LOGW(TAG, "get_work HTTP failed (%d)", n);
        return -2;
    }

    /* Parse JSON response */
    memset(tmpl, 0, sizeof(*tmpl));

    json_find_hex(response, "header", tmpl->header, 80);
    json_find_int(response, "target_bits", (int *)&tmpl->target_bits);
    json_find_int(response, "height", (int *)&tmpl->height);
    json_find_string(response, "job_id", tmpl->job_id, sizeof(tmpl->job_id));

    return 0;
}

int quartz_mining_get_work(qz_block_template_t *tmpl) {
    const char *addr = quartz_wallet_get_address();
    int rc = quartz_mining_get_work_for(addr ? addr : "", tmpl);
    if (rc != 0) return rc;

    g_mining_state = QZ_MINING_HAS_WORK;
    /* Log only on new block height — job refreshes of the same block stay quiet */
    static int s_last_logged_height = -1;
    if ((int)tmpl->height != s_last_logged_height) {
        s_last_logged_height = (int)tmpl->height;
        ESP_LOGI(TAG, "Got work: height=%d target=%d", tmpl->height, tmpl->target_bits);
    }

    return 0;
}

int quartz_mining_submit(const char *job_id, uint64_t nonce, const uint8_t header[80]) {
    if (!quartz_wifi_is_connected()) return -1;

    /* Get wallet address and current hashrate for node tracking */
    const char *addr = quartz_wallet_get_address();
    extern uint32_t g_last_hps;  /* from main.c mining loop */

    /* Build JSON body */
    char body[768];
    char header_hex[161];
    for (int i = 0; i < 80; i++) {
        snprintf(header_hex + i * 2, 3, "%02x", header[i]);
    }
    header_hex[160] = '\0';

    snprintf(body, sizeof(body),
        "{\"job_id\":\"%s\",\"nonce\":%llu,\"header\":\"%s\",\"address\":\"%s\",\"hashrate\":%lu}",
        job_id, (unsigned long long)nonce, header_hex, addr ? addr : "", (unsigned long)g_last_hps);

    char response[512];
    int n = http_request("POST", NODE_PATH_SUBMIT, body, response, sizeof(response));
    if (n <= 0) {
        ESP_LOGW(TAG, "submit HTTP failed (%d)", n);
        return -2;
    }

    /* Check for acceptance */
    char status[32];
    if (json_find_string(response, "status", status, sizeof(status))) {
        if (strcmp(status, "accepted") == 0) {
            ESP_LOGI(TAG, "✅ Share accepted!");
            g_mining_state = QZ_MINING_SUBMITTED;
            return 0;
        } else {
            ESP_LOGW(TAG, "❌ Share rejected: %s", status);
            return -3;
        }
    }

    return 0;
}

bool quartz_mining_node_reachable(void) {
    char response[256];
    int n = http_request("GET", NODE_PATH_INFO, NULL, response, sizeof(response));
    return n > 0;
}

/* ============================================================
 * Messaging Protocol
 * ============================================================ */

#define NODE_PATH_MESSAGES "/api/v1/messages"
#define NODE_PATH_MSG_SEND "/api/v1/messages/send"

int quartz_messages_get_latest(qz_message_t *msg) {
    if (!quartz_wifi_is_connected()) return -1;

    char response[4096];
    int n = http_request("GET", NODE_PATH_MESSAGES, NULL, response, sizeof(response));
    if (n <= 0) return -2;

    /* Find first message in the JSON array */
    memset(msg, 0, sizeof(*msg));

    char *p = strstr(response, "\"messages\"");
    if (!p) return -3;

    /* Find first { after messages array */
    p = strchr(p, '{');
    if (!p) return -4;

    /* Parse fields from this message object */
    json_find_string(p, "from", msg->from, sizeof(msg->from));
    json_find_string(p, "data_text", msg->text, sizeof(msg->text));

    int bh = 0;
    json_find_int(p, "block", &bh);
    msg->block_height = bh;

    /* Check for confirmed field */
    char conf[16];
    if (json_find_string(p, "confirmed", conf, sizeof(conf))) {
        msg->confirmed = (strcmp(conf, "true") == 0);
    } else {
        msg->confirmed = (bh > 0);
    }

    /* If no text found, no messages */
    if (msg->text[0] == '\0') return -5;

    ESP_LOGI(TAG, "Got message: from=%s text='%.40s'", msg->from, msg->text);
    return 0;
}

int quartz_messages_send(const char *from, const char *to, const char *text) {
    if (!quartz_wifi_is_connected()) return -1;
    if (!text || strlen(text) == 0 || strlen(text) > 160) return -2;

    /* Build JSON body */
    char body[512];
    snprintf(body, sizeof(body),
        "{\"from\":\"%s\",\"to\":\"%s\",\"text\":\"%s\"}",
        from ? from : "", to ? to : "", text);

    char response[512];
    int n = http_request("POST", NODE_PATH_MSG_SEND, body, response, sizeof(response));
    if (n <= 0) {
        ESP_LOGW(TAG, "Message send failed (%d)", n);
        return -3;
    }

    /* Check for queued status */
    char status[32];
    if (json_find_string(response, "status", status, sizeof(status))) {
        if (strcmp(status, "queued") == 0) {
            ESP_LOGI(TAG, "Message queued for next block!");
            return 0;
        }
    }

    return 0;
}

/* ============================================================
 * Captive Portal Seed Phrase Provisioning
 * ============================================================ */

void quartz_wifi_portal_set_seed(const char words[12][12], const char *address) {
    memcpy(s_portal_seed, words, sizeof(s_portal_seed));
    strncpy(s_portal_address, address ? address : "", sizeof(s_portal_address) - 1);
    s_portal_seed_available = true;
    s_portal_seed_confirmed = false;
    /* Pick random challenge word for verification */
    s_portal_challenge_idx = esp_random() % 12;
    ESP_LOGI(TAG, "Seed phrase loaded into captive portal (visit /seed, challenge word #%d)",
             s_portal_challenge_idx + 1);
}

bool quartz_wifi_portal_seed_confirmed(void) {
    return s_portal_seed_confirmed;
}

#endif /* ESP_PLATFORM */
