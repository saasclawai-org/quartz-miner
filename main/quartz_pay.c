/**
 * quartz_pay.c — QR Payments + Relay Control
 *
 * The real-world use case: someone pays QZ, a physical thing happens.
 *
 * Flow:
 * 1. quartz_pay_init() — set up GPIO relay, store wallet address
 * 2. quartz_pay_request(0.50, "Coffee") — display QR, start polling
 * 3. quartz_pay_poll() — check node for incoming tx to our address
 * 4. Payment confirmed → relay triggers → thing happens
 *
 * The QR code encodes: quartz:<address>?amount=<QZ>&label=<text>
 * A phone wallet app would parse this and present a payment UI.
 */

#include "quartz_pay.h"
#include "quartz_qr.h"
#include "quartz_display.h"
#include "quartz_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>   /* v089.12: strcasecmp/strncasecmp for relay_cmd */

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_system.h"   /* v089.12: esp_restart for relay pin change */

static const char *TAG = "QZ.PAY";
#endif

static qz_pay_request_t s_request;
static char s_wallet_address[65] = {0};
static bool s_initialized = false;
/* v079: runtime overrides (NVS "qz_relay": pin u8, dur u32) */
static uint8_t  s_relay_pin   = QZ_PAY_RELAY_PIN;
static uint32_t s_duration_ms = QZ_PAY_RELAY_DURATION_MS;

/* v080: 0-conf fire mode + polarity invert */
static bool s_fast_mode = true;     /* true = fire on pending (0-conf) */
static bool s_invert    = false;    /* swap active/idle GPIO levels */
static bool s_auto_rearm = false;   /* v082: vending mode — re-arm after each fire */
static uint64_t s_boot_restore_sats = 0;  /* v083: NVS-persisted armed watch, restored on first poll */

/* v083: persist armed state so a rebooting vending machine re-arms itself */
static void pay_save_armed_nvs(bool armed, uint64_t price_sats) {
#ifdef ESP_PLATFORM
    nvs_handle_t h;
    if (nvs_open("qz_relay", NVS_READWRITE, &h) == ESP_OK) {
        if (armed) {
            nvs_set_u8(h, "armed", 1);
            nvs_set_u64(h, "price", price_sats);
        } else {
            nvs_erase_key(h, "armed");
        }
        nvs_commit(h);
        nvs_close(h);
    }
#endif
}

#define QZ_PAY_MAX_DURATION_MS 86400000UL   /* v080: 24 h hold cap */

/* Shared HTTP response buffer — the arm-time snapshot (console task) finishes
 * before state becomes QZ_PAY_WAITING, so it never overlaps the poll (mining
 * loop). One writer at a time. */
static char s_http_buf[2048];

static int relay_active_level(void) {
    int lvl = QZ_PAY_RELAY_ACTIVE_LOW ? 0 : 1;
    return s_invert ? (lvl ? 0 : 1) : lvl;
}
static void relay_write(bool on) {
#ifdef ESP_PLATFORM
    gpio_set_level(s_relay_pin, on ? relay_active_level() : (relay_active_level() ? 0 : 1));
#else
    (void)on;
#endif
}

#ifdef ESP_PLATFORM
/* esp_timer — already linked & running (WiFi/system use it). FreeRTOS timers
 * would drag ~1.4 KB of IRAM-resident API into the image and overflow the
 * classic ESP32's IRAM. */
static esp_timer_handle_t s_relay_timer = NULL;
static void relay_off_cb(void *arg) {
    (void)arg;
    relay_write(false);
    ESP_LOGI(TAG, "Relay released");
}
#endif

/* === Node API for payment checking === */
/* The reference node exposes:
 *   GET /api/v1/address/<address>/txs
 * Returns: { "txs": [ { "txid": "...", "amount": 5000000000, "confirmations": 3 } ] }
 */

int quartz_pay_init(const char *wallet_address) {
    if (!wallet_address) return -1;

    strncpy(s_wallet_address, wallet_address, sizeof(s_wallet_address) - 1);

#ifdef ESP_PLATFORM
    /* v079/v080: load NVS overrides */
    nvs_handle_t nh;
    if (nvs_open("qz_relay", NVS_READONLY, &nh) == ESP_OK) {
        uint8_t pin = 0; uint32_t dur = 0; uint8_t mode = 0, inv = 0, aut = 0, armed = 0;
        uint64_t rprice = 0;
        if (nvs_get_u8(nh, "pin", &pin) == ESP_OK && pin < 48) s_relay_pin = pin;
        if (nvs_get_u32(nh, "dur", &dur) == ESP_OK && dur >= 100 && dur <= QZ_PAY_MAX_DURATION_MS) s_duration_ms = dur;
        if (nvs_get_u8(nh, "mode", &mode) == ESP_OK) s_fast_mode = (mode == 0);
        if (nvs_get_u8(nh, "inv", &inv) == ESP_OK) s_invert = (inv != 0);
        if (nvs_get_u8(nh, "auto", &aut) == ESP_OK) s_auto_rearm = (aut != 0);
        if (nvs_get_u8(nh, "armed", &armed) == ESP_OK && armed) {
            if (nvs_get_u64(nh, "price", &rprice) == ESP_OK && rprice > 0)
                s_boot_restore_sats = rprice;
        }
        nvs_close(nh);
    }
    /* Configure relay GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << s_relay_pin),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Start with relay OFF */
    relay_write(false);

    ESP_LOGI(TAG, "Payment system ready, relay on GPIO%d · %s · invert %s · auto %s",
             s_relay_pin,
             s_fast_mode ? "fast (0-conf)" : "safe (1 conf)",
             s_invert ? "on" : "off",
             s_auto_rearm ? "on" : "off");
    ESP_LOGI(TAG, "Wallet: %s", s_wallet_address);
    if (s_boot_restore_sats > 0)
        ESP_LOGI(TAG, "v083: persisted watch found — will re-arm %.2f QZ once network is up",
                 (float)s_boot_restore_sats / 1e8f);
#endif

    s_request.state = QZ_PAY_IDLE;
    s_initialized = true;
    return 0;
}

int quartz_pay_build_qr_string(
    char *buf, int buf_len,
    const char *address,
    float amount_qz,
    const char *label
) {
    if (!buf || !address || buf_len < 80) return -1;

    /* Format: quartz:<address>?amount=<QZ>&label=<text>
     * Keep it short for QR efficiency */

    int offset = snprintf(buf, buf_len, "quartz:%s?amount=%.8f", address, amount_qz);

    if (label && label[0] && offset < buf_len - 20) {
        offset += snprintf(buf + offset, buf_len - offset, "&label=%s", label);
    }

    return offset;
}

/* ---- v080: per-entry JSON helpers ----
 * Node /txs lists confirmed entries (oldest first) then pending (0-conf)
 * last. Each entry: {"txid": "…", "amount": N, "confirmations": n, "pending": bool} */
static void copy_txid16(char dst[17], const char *v, const char *end) {
    int i = 0;
    while (v + i < end && v[i] != '"' && i < 16) { dst[i] = v[i]; i++; }
    dst[i] = '\0';
}

/* Remember every txid in the arm-time response — only payments NOT already
 * known will trigger. Runs before state=WAITING so the mining-loop poll
 * can't race the snapshot. v081: http_request returns body length (>0) on
 * success — the v079/v080 checks were inverted and never parsed anything. */
static void quartz_pay_snapshot_known_txid(void) {
    s_request.known_txid_count = 0;
    memset(s_request.known_txids, 0, sizeof(s_request.known_txids));
#ifdef ESP_PLATFORM
    char path[192];
    snprintf(path, sizeof(path), "/api/v1/address/%s/txs?min_amount=%llu",
             s_request.address, (unsigned long long)s_request.amount_satoshis);
    int rc = quartz_http_request("GET", path, NULL, s_http_buf, sizeof(s_http_buf));
    if (rc <= 0) {
        ESP_LOGW(TAG, "Arm snapshot HTTP failed (rc=%d) — any qualifying tx will fire", rc);
        return;
    }
    const char *p = strstr(s_http_buf, "\"txid\"");
    while (p && s_request.known_txid_count < 6) {
        const char *v = strchr(p + 6, '"');
        if (!v) break;
        v++;
        int i = 0;
        while (v[i] && v[i] != '"' && i < 16) {
            s_request.known_txids[s_request.known_txid_count][i] = v[i];
            i++;
        }
        s_request.known_txids[s_request.known_txid_count][i] = '\0';
        s_request.known_txid_count++;
        p = strstr(v, "\"txid\"");
    }
    ESP_LOGI(TAG, "Arm snapshot: %d known txids (newest %.16s)",
             s_request.known_txid_count,
             s_request.known_txid_count ? s_request.known_txids[s_request.known_txid_count - 1] : "(none)");
#endif
}

int quartz_pay_request(float amount_qz, const char *label) {
    if (!s_initialized) return -1;

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Payment request: %.8f QZ%s%s",
             amount_qz, label ? " for " : "", label ? label : "");
#endif

    /* Build QR string */
    char qr_str[256];
    int len = quartz_pay_build_qr_string(qr_str, sizeof(qr_str),
                                         s_wallet_address, amount_qz, label);
    if (len < 0) return -1;

    /* Set up request state */
    memset(&s_request, 0, sizeof(s_request));
    strncpy(s_request.address, s_wallet_address, sizeof(s_request.address) - 1);
    s_request.amount_satoshis = (uint64_t)(amount_qz * 1e8);

    if (label) {
        strncpy(s_request.label, label, sizeof(s_request.label) - 1);
    }

#ifdef ESP_PLATFORM
    /* Set timeout */
    s_request.created_time = esp_timer_get_time() / 1000000;
    s_request.expires_time = s_request.created_time + QZ_PAY_TIMEOUT_S;

    /* Display QR code centered on screen */
    int qr_size = qr_modules(quartz_qr_version_for_data(len, QR_ECC_MEDIUM));
    int scale = 2;
    int total_px = qr_size * scale;
    int x = (320 - total_px) / 2;  /* center horizontally */
    int y = 30;                     /* below header */

    quartz_display_clear(0x0843);  /* dark background */

    /* Header */
    quartz_display_fill_rect(0, 0, 320, 22, 0x8410);  /* dark purple */
    char header[64];
    snprintf(header, sizeof(header), "Pay %.2f QZ", amount_qz);
    quartz_display_draw_text(8, 3, header, 0xFFFF, 0x8410);

    /* QR code */
    int rc = quartz_qr_display(qr_str, QR_ECC_MEDIUM, x, y, scale,
                               0x0000, 0xFFFF);  /* black on white */

    if (rc != 0) {
        ESP_LOGE(TAG, "QR generation failed");
        s_request.state = QZ_PAY_ERROR;
        return -1;
    }

    /* Address below QR */
    char addr_short[20];
    snprintf(addr_short, sizeof(addr_short), "%.8s...%.6s",
             s_wallet_address, s_wallet_address + strlen(s_wallet_address) - 6);
    quartz_display_draw_text(8, y + total_px + 8, addr_short, 0x8888, 0x0843);

    /* Label if present */
    if (label) {
        char label_str[40];
        snprintf(label_str, sizeof(label_str), "Item: %s", label);
        quartz_display_draw_text(8, y + total_px + 24, label_str, 0xFFFF, 0x0843);
    }

    /* Instructions */
    quartz_display_draw_text(8, 220, "Scan QR to pay QZ", 0x8888, 0x0843);

    ESP_LOGI(TAG, "QR displayed: %s", qr_str);
#endif

    /* v080: snapshot existing txs, THEN arm — only new payments fire */
    quartz_pay_snapshot_known_txid();
    s_request.state = QZ_PAY_WAITING;
    pay_save_armed_nvs(true, s_request.amount_satoshis);

    return 0;
}

qz_pay_state_t quartz_pay_poll(void) {
    /* v083: restore NVS-persisted watch on first poll (network up → the arm
     * snapshot sees the real tx list, so no stale payments retro-fire). */
    if (s_boot_restore_sats > 0 && s_request.state == QZ_PAY_IDLE) {
        float price_qz = (float)s_boot_restore_sats / 1e8f;
        s_boot_restore_sats = 0;
        ESP_LOGI(TAG, "v083: re-arming persisted watch (%.2f QZ)", price_qz);
        quartz_pay_request(price_qz, "relay");
        return s_request.state;
    }
    if (!s_initialized || s_request.state != QZ_PAY_WAITING) {
        return s_request.state;
    }

#ifdef ESP_PLATFORM
    /* Check timeout */
    uint32_t now = esp_timer_get_time() / 1000000;
    /* v082: vending mode never expires — it re-arms after every payment */
    if (!s_auto_rearm && now > s_request.expires_time) {
        ESP_LOGW(TAG, "Payment request expired");
        pay_save_armed_nvs(false, 0);
        s_request.state = QZ_PAY_EXPIRED;
        return s_request.state;
    }

    /* Check node for incoming payment — v080: ask only for relevant amounts */
    char path[192];
    snprintf(path, sizeof(path), "/api/v1/address/%s/txs?min_amount=%llu",
             s_request.address, (unsigned long long)s_request.amount_satoshis);

    /* v080: shared buffer (see s_http_buf note) */
    int rc = quartz_http_request("GET", path, NULL, s_http_buf, sizeof(s_http_buf));

    if (rc <= 0) {
        /* v081: http_request returns body length (>0) on success. v079/v080
         * checked == 0 — i.e. only parsed when the node returned NOTHING.
         * Payment detection has been dead since v079 shipped. */
        ESP_LOGW(TAG, "Payment poll HTTP failed (rc=%d)", rc);
    }
    if (rc > 0) {
        /* v080: per-entry walk. Confirmed entries arrive oldest-first, then
         * pending (0-conf) last. Fire on any NEW qualifying tx:
         *   fast mode (default): pending or confirmed — fires in ~1 poll
         *   safe mode:           confirmations >= QZ_PAY_CONFIRMATIONS      */
        const char *e = strstr(s_http_buf, "\"txs\"");
        if (!e) e = s_http_buf;
        while ((e = strstr(e, "\"txid\"")) != NULL) {
            const char *v = strchr(e + 6, '"');
            if (!v) break;
            v++;                                        /* first char of txid */
            const char *next = strstr(v, "\"txid\"");   /* next entry start */
            const char *end  = next ? next : (s_http_buf + strlen(s_http_buf));

            char txid16[17];
            copy_txid16(txid16, v, end);

            uint64_t amount = 0;
            const char *pa = strstr(v, "\"amount\"");
            if (pa && pa < end) {
                pa = strchr(pa + 8, ':');
                if (pa && pa < end) amount = strtoull(pa + 1, NULL, 10);
            }
            int confirmations = 0;
            const char *pc = strstr(v, "\"confirmations\"");
            if (pc && pc < end) {
                pc = strchr(pc + 15, ':');
                if (pc && pc < end) confirmations = atoi(pc + 1);
            }
            bool pending = false;
            const char *pp = strstr(v, "\"pending\"");
            if (pp && pp < end) {
                pp = strchr(pp + 9, ':');
                if (pp && pp < end) {
                    pp++;
                    while (*pp == ' ') pp++;
                    pending = (*pp == 't');
                }
            }

            /* v082: coinbase filter — mining rewards to our own address carry
             * "counterparty": null and must never fire the relay (otherwise
             * auto mode self-triggers on every block we mine). Real incoming
             * payments carry the sender's address string. */
            bool is_coinbase = false;
            const char *pcp = strstr(v, "\"counterparty\"");
            if (pcp && pcp < end) {
                const char *cc = strchr(pcp + 14, ':');
                if (cc && cc < end) {
                    cc++;
                    while (*cc == ' ') cc++;
                    is_coinbase = (strncmp(cc, "null", 4) == 0);
                }
            }

            bool is_new = true;
            for (int k = 0; k < s_request.known_txid_count; k++) {
                if (strncmp(txid16, s_request.known_txids[k], 16) == 0) { is_new = false; break; }
            }
            bool policy_ok = s_fast_mode ? true : (confirmations >= QZ_PAY_CONFIRMATIONS);

            if (!is_coinbase && is_new && amount >= s_request.amount_satoshis && policy_ok) {
                ESP_LOGI(TAG, "Payment received: %llu sats (needed %llu) %s txid %.16s",
                         (unsigned long long)amount,
                         (unsigned long long)s_request.amount_satoshis,
                         pending ? "[0-conf]" : "[confirmed]", txid16);
                strncpy(s_request.tx_hash, txid16, sizeof(s_request.tx_hash) - 1);
                s_request.tx_hash[sizeof(s_request.tx_hash) - 1] = '\0';

                /* Trigger relay! (non-blocking — timer releases it) */
                quartz_pay_trigger_relay(s_duration_ms);

                /* Update display */
                quartz_display_clear(0x0843);
                quartz_display_fill_rect(0, 0, 320, 22, 0x04E0);  /* green */
                quartz_display_draw_text(8, 3, "PAID - Thank you!", 0x0000, 0x04E0);

                char amt_str[32];
                snprintf(amt_str, sizeof(amt_str), "%.2f QZ received",
                         (float)s_request.amount_satoshis / 1e8);
                quartz_display_draw_text(8, 40, amt_str, 0x04E0, 0x0843);

                quartz_display_draw_text(8, 70, "Relay activated!", 0xFFFF, 0x0843);

                char tx_str[40];
                snprintf(tx_str, sizeof(tx_str), "TX: %.36s...", s_request.tx_hash);
                quartz_display_draw_text(8, 100, tx_str, 0x8888, 0x0843);

                if (s_auto_rearm) {
                    /* v082: vending mode — re-arm for the next customer.
                     * request() memsets s_request (save amount/label first)
                     * and takes a fresh arm snapshot; the payment that just
                     * fired is in that snapshot, so it can't double-fire. */
                    uint64_t amt = s_request.amount_satoshis;
                    char lbl[33];
                    strncpy(lbl, s_request.label, sizeof(lbl) - 1);
                    lbl[sizeof(lbl) - 1] = '\0';
                    quartz_pay_request((float)amt / 1e8, lbl);
                    ESP_LOGI(TAG, "⚡ Auto re-armed — watching for %.2f QZ (mining rewards ignored)",
                             (float)amt / 1e8);
                } else {
                    pay_save_armed_nvs(false, 0);   /* v083: one-shot consumed */
                    s_request.state = QZ_PAY_CONFIRMED;
                }
                break;
            }
            if (!next) break;
            e = next;
        }
    }
#endif

    return s_request.state;
}

void quartz_pay_trigger_relay(uint32_t duration_ms) {
    if (duration_ms == 0) duration_ms = s_duration_ms;

#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Triggering relay (GPIO%d) for %llu ms (non-blocking)",
             s_relay_pin, (unsigned long long)duration_ms);

    relay_write(true);
    s_request.relay_trigger_time = esp_timer_get_time() / 1000;

    /* v080: one-shot esp_timer releases the relay — works whether or not the
     * mining loop is running, and long holds no longer block it */
    if (!s_relay_timer) {
        const esp_timer_create_args_t args = {
            .callback = relay_off_cb,
            .name = "qz_relay",
        };
        esp_timer_create(&args, &s_relay_timer);
    }
    if (s_relay_timer) {
        esp_timer_start_once(s_relay_timer, (uint64_t)duration_ms * 1000ULL);
    }
#endif
}

void quartz_pay_cancel(void) {
    pay_save_armed_nvs(false, 0);
    s_request.state = QZ_PAY_IDLE;
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Payment request cancelled");
#endif
}

qz_pay_state_t quartz_pay_get_state(void) {
    return s_request.state;
}

const qz_pay_request_t *quartz_pay_get_request(void) {
    return &s_request;
}

/* v089.12: BLE/CLI guard — quartz_pay_init() resets state to IDLE,
 * so transport layers init once; blind re-init disarms an armed watch. */
bool quartz_pay_is_initialized(void) {
    return s_initialized;
}


/* ---- v079 additions ---- */
void quartz_pay_set_duration_ms(uint32_t duration_ms) {
    if (duration_ms < 100) duration_ms = QZ_PAY_RELAY_DURATION_MS;
    if (duration_ms > QZ_PAY_MAX_DURATION_MS) duration_ms = QZ_PAY_MAX_DURATION_MS;
    s_duration_ms = duration_ms;
    nvs_handle_t h;
    if (nvs_open("qz_relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "dur", s_duration_ms);
        nvs_commit(h);
        nvs_close(h);
    }
}
uint32_t quartz_pay_get_duration_ms(void) { return s_duration_ms; }
uint8_t quartz_pay_get_pin(void) { return s_relay_pin; }

/* ---- v080 additions ---- */
void quartz_pay_set_fast(bool fast) {
    s_fast_mode = fast;
    nvs_handle_t h;
    if (nvs_open("qz_relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "mode", fast ? 0 : 1);
        nvs_commit(h);
        nvs_close(h);
    }
}
bool quartz_pay_get_fast(void) { return s_fast_mode; }

/* v082: auto re-arm (vending mode) */
void quartz_pay_set_auto(bool auto_rearm) {
    s_auto_rearm = auto_rearm;
    nvs_handle_t h;
    if (nvs_open("qz_relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "auto", auto_rearm ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}
bool quartz_pay_get_auto(void) { return s_auto_rearm; }

void quartz_pay_toggle_invert(void) {
    s_invert = !s_invert;
    nvs_handle_t h;
    if (nvs_open("qz_relay", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "inv", s_invert ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
    relay_write(false);   /* re-apply idle level immediately */
}
bool quartz_pay_get_invert(void) { return s_invert; }

/* ---- v089.12 additions: relay over BLE (docs/RELAY-BLE-SPEC.md) ---- */

void quartz_pay_set_invert(bool invert) {
    if (s_invert == invert) return;   /* no-op: skip NVS churn */
    quartz_pay_toggle_invert();       /* persists + re-applies idle level */
}

/* One-line status shared by CLI reply and BLE command ack */
static void relay_status_line(char *reply, size_t reply_len) {
    const qz_pay_request_t *rq = quartz_pay_get_request();
    bool armed = (quartz_pay_get_state() == QZ_PAY_WAITING);
    int n = snprintf(reply, reply_len,
                     "Relay: pin GPIO%d \u00b7 pulse %lums \u00b7 %s \u00b7 invert %s \u00b7 auto %s \u00b7 %s",
                     (int) quartz_pay_get_pin(),
                     (unsigned long) quartz_pay_get_duration_ms(),
                     quartz_pay_get_fast() ? "fast (0-conf)" : "safe (1 conf)",
                     quartz_pay_get_invert() ? "on" : "off",
                     quartz_pay_get_auto() ? "on" : "off",
                     armed ? "ARMED" : "not armed");
    if (armed && rq && rq->amount_satoshis > 0 && n > 0 && (size_t) n < reply_len - 16) {
        snprintf(reply + n, reply_len - n, " (%.2f QZ)",
                 (float) rq->amount_satoshis / 1e8f);
    }
}

int quartz_pay_relay_cmd(const char *line, char *reply, size_t reply_len) {
    if (!reply || reply_len < 64) return -1;
    reply[0] = '\0';
    if (!line) line = "";
    while (*line == ' ') line++;

    if (*line == '\0') {                       /* status */
        relay_status_line(reply, reply_len);
        return 0;
    }
    if (strncasecmp(line, "test", 4) == 0) {   /* test [sec] */
        int sec = atoi(line + 4);
        snprintf(reply, reply_len, "\u26a1 Firing relay NOW (test, %ds)",
                 sec > 0 ? sec : (int)(quartz_pay_get_duration_ms() / 1000));
        quartz_pay_trigger_relay(sec > 0 ? (uint32_t) sec * 1000 : 0);
        return 0;
    }
    if (strcasecmp(line, "off") == 0 || strcasecmp(line, "cancel") == 0) {
        quartz_pay_cancel();
        snprintf(reply, reply_len, "Relay watch cancelled");
        return 0;
    }
    if (strncasecmp(line, "fast", 4) == 0 || strcasecmp(line, "safe") == 0) {
        bool fast = (strncasecmp(line, "fast", 4) == 0);
        const char *arg = line + 4;            /* both keywords are 4 chars */
        if (*arg == ' ' && arg[1] != '\0') fast = (atoi(arg + 1) != 0);
        quartz_pay_set_fast(fast);
        snprintf(reply, reply_len, "Relay mode: %s",
                 quartz_pay_get_fast()
                     ? "fast \u2014 fire on 0-conf (~2s)" : "safe \u2014 wait 1 block confirmation");
        return 0;
    }
    if (strcasecmp(line, "auto") == 0 || strncasecmp(line, "auto ", 5) == 0) {
        bool on = (line[4] == '\0') ? !quartz_pay_get_auto() : (atoi(line + 5) != 0);
        quartz_pay_set_auto(on);
        snprintf(reply, reply_len, "Relay auto re-arm %s \u2014 %s", on ? "ON" : "OFF",
                 on ? "re-arms after every payment, never expires; mining rewards ignored"
                     : "one-shot per arming (300s timeout)");
        return 0;
    }
    if (strcasecmp(line, "invert") == 0 || strncasecmp(line, "invert ", 7) == 0) {
        bool on = (line[6] == '\0') ? !quartz_pay_get_invert() : (atoi(line + 7) != 0);
        quartz_pay_set_invert(on);
        snprintf(reply, reply_len, "Relay invert %s \u2014 module treated as active-%s",
                 on ? "ON" : "OFF", on ? "LOW" : "HIGH");
        return 0;
    }
#ifdef ESP_PLATFORM
    if (strncasecmp(line, "pin ", 4) == 0) {
        int rp = atoi(line + 4);
        if (rp < 0 || rp > 48) {
            snprintf(reply, reply_len, "Usage: pin <0-48>");
            return -1;
        }
        nvs_handle_t rh;
        if (nvs_open("qz_relay", NVS_READWRITE, &rh) == ESP_OK) {
            nvs_set_u8(rh, "pin", (uint8_t) rp);
            nvs_commit(rh);
            nvs_close(rh);
            snprintf(reply, reply_len, "Relay pin set to GPIO%d \u2014 rebooting\u2026", rp);
            vTaskDelay(pdMS_TO_TICKS(500));
            esp_restart();
        }
        snprintf(reply, reply_len, "Relay pin write failed");
        return -1;
    }
#endif
    /* arm <price_qz> [pulse_s] [fast|safe] — bare "<price> \u2026" also accepted */
    {
        const char *p = line;
        if (strncasecmp(line, "arm ", 4) == 0) {
            p = line + 4;
            while (*p == ' ') p++;
        }
        if (*p >= '0' && *p <= '9') {
            float price = strtof(p, NULL);
            if (price > 0.0f && price <= 100000.0f) {
                const char *rsp = strchr(p, ' ');
                if (rsp) {
                    int dsec = atoi(rsp);
                    if (dsec > 0) quartz_pay_set_duration_ms((uint32_t) dsec * 1000);
                    /* v080: optional [fast|safe] after the seconds */
                    const char *tok = dsec > 0 ? strchr(rsp + 1, ' ') : rsp;
                    if (tok) {
                        while (*tok == ' ') tok++;
                        if ((tok[0]=='f'||tok[0]=='F') && (tok[1]=='a'||tok[1]=='A')) quartz_pay_set_fast(true);
                        else if ((tok[0]=='s'||tok[0]=='S') && (tok[1]=='a'||tok[1]=='A')) quartz_pay_set_fast(false);
                    }
                }
                char ruri[160];
                quartz_pay_build_qr_string(ruri, sizeof(ruri), s_wallet_address, price, "relay");
                snprintf(reply, reply_len,
                         "\u26a1 armed %.2f QZ \u00b7 pulse %lus \u00b7 %s \u00b7 fires on %s",
                         price,
                         (unsigned long)(quartz_pay_get_duration_ms() / 1000),
                         ruri,
                         quartz_pay_get_fast() ? "0-conf (~2s)" : "1 confirmation");
                quartz_pay_request(price, "relay");
                return 0;
            }
        }
    }
    snprintf(reply, reply_len,
             "Usage: arm <qz> [pulse_s] [fast|safe] | test [sec] | off | fast [1|0] | invert [1|0] | auto [1|0] | pin <gpio>");
    return -1;
}

int quartz_pay_build_relay_json(char *buf, size_t buf_len) {
    if (!buf || buf_len < 200) return -1;
    static const char *names[] = {
        "idle", "armed", "receiving", "fired", "expired", "error"
    };
    qz_pay_state_t st = quartz_pay_get_state();
    const qz_pay_request_t *rq = quartz_pay_get_request();
    float price = (rq && rq->amount_satoshis > 0) ? (float) rq->amount_satoshis / 1e8f : 0.0f;
    int n = snprintf(buf, buf_len,
        "{\"v\":1,\"state\":\"%s\",\"price_qz\":%.8f,\"pulse_s\":%lu,"
        "\"fast\":%s,\"invert\":%s,\"auto\":%s,\"pin\":%d,\"addr\":\"%s\"",
        (st >= QZ_PAY_IDLE && st <= QZ_PAY_ERROR) ? names[st] : "error",
        price,
        (unsigned long)(quartz_pay_get_duration_ms() / 1000),
        quartz_pay_get_fast() ? "true" : "false",
        quartz_pay_get_invert() ? "true" : "false",
        quartz_pay_get_auto() ? "true" : "false",
        (int) quartz_pay_get_pin(),
        s_wallet_address);
    if (n < 0 || (size_t) n >= buf_len) return -1;
    if (st == QZ_PAY_WAITING) {
        char uri[160];
        quartz_pay_build_qr_string(uri, sizeof(uri), s_wallet_address, price, "relay");
        n += snprintf(buf + n, buf_len - n, ",\"uri\":\"%s\"}", uri);
    } else {
        n += snprintf(buf + n, buf_len - n, "}");
    }
    return (n > 0 && (size_t) n < buf_len) ? n : -1;
}
