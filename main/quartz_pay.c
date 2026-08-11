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
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QZ.PAY";
#endif

static qz_pay_request_t s_request;
static char s_wallet_address[65] = {0};
static bool s_initialized = false;

/* === Node API for payment checking === */
/* The reference node exposes:
 *   GET /api/v1/address/<address>/txs
 * Returns: { "txs": [ { "txid": "...", "amount": 5000000000, "confirmations": 3 } ] }
 */

int quartz_pay_init(const char *wallet_address) {
    if (!wallet_address) return -1;

    strncpy(s_wallet_address, wallet_address, sizeof(s_wallet_address) - 1);

#ifdef ESP_PLATFORM
    /* Configure relay GPIO */
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << QZ_PAY_RELAY_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    /* Start with relay OFF */
    int level = QZ_PAY_RELAY_ACTIVE_LOW ? 1 : 0;
    gpio_set_level(QZ_PAY_RELAY_PIN, level);

    ESP_LOGI(TAG, "Payment system ready, relay on GPIO%d", QZ_PAY_RELAY_PIN);
    ESP_LOGI(TAG, "Wallet: %s", s_wallet_address);
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
    s_request.state = QZ_PAY_WAITING;
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

    return 0;
}

qz_pay_state_t quartz_pay_poll(void) {
    if (!s_initialized || s_request.state != QZ_PAY_WAITING) {
        return s_request.state;
    }

#ifdef ESP_PLATFORM
    /* Check timeout */
    uint32_t now = esp_timer_get_time() / 1000000;
    if (now > s_request.expires_time) {
        ESP_LOGW(TAG, "Payment request expired");
        s_request.state = QZ_PAY_EXPIRED;
        return s_request.state;
    }

    /* Check node for incoming payment */
    /* Build API URL: /api/v1/address/<addr>/txs */
    char url[256];
    snprintf(url, sizeof(url), "http://quartz.preview.saasclaw.ai/api/v1/address/%s/txs",
             s_request.address);

    /* Use the WiFi HTTP client to check for payments */
    /* For now, we do a simple GET and look for matching amount */
    char response[1024];
    int rc = quartz_wifi_http_get(url, response, sizeof(response));

    if (rc == 0) {
        /* Parse JSON response looking for amount */
        /* Simple string search for "amount" field */
        char *p = strstr(response, "\"amount\"");
        if (p) {
            /* Extract amount value */
            p = strchr(p + 8, ':');
            if (p) {
                p++;
                uint64_t amount = strtoull(p, NULL, 10);

                if (amount >= s_request.amount_satoshis) {
                    /* Payment received! */
                    ESP_LOGI(TAG, "Payment received: %llu sats (needed %llu)",
                             amount, s_request.amount_satoshis);

                    /* Extract tx hash */
                    char *txid = strstr(response, "\"txid\"");
                    if (txid) {
                        txid = strchr(txid + 6, '"');
                        if (txid) {
                            txid++;
                            char *end = strchr(txid, '"');
                            int len = end - txid;
                            if (len > 0 && len < 64) {
                                memcpy(s_request.tx_hash, txid, len);
                                s_request.tx_hash[len] = '\0';
                            }
                        }
                    }

                    /* Check confirmations */
                    char *conf = strstr(response, "\"confirmations\"");
                    int confirmations = 0;
                    if (conf) {
                        conf = strchr(conf + 15, ':');
                        if (conf) {
                            confirmations = atoi(conf + 1);
                        }
                    }

                    if (confirmations >= QZ_PAY_CONFIRMATIONS) {
                        ESP_LOGI(TAG, "Payment confirmed (%d confirmations)", confirmations);
                        s_request.state = QZ_PAY_CONFIRMED;

                        /* Trigger relay! */
                        quartz_pay_trigger_relay(QZ_PAY_RELAY_DURATION_MS);

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
                    } else {
                        ESP_LOGI(TAG, "Payment detected, %d/%d confirmations",
                                 confirmations, QZ_PAY_CONFIRMATIONS);
                        s_request.state = QZ_PAY_RECEIVED;
                    }
                }
            }
        }
    }
#endif

    return s_request.state;
}

void quartz_pay_trigger_relay(uint32_t duration_ms) {
    if (duration_ms == 0) duration_ms = QZ_PAY_RELAY_DURATION_MS;

#ifdef ESP_PLATFORM
    int active_level = QZ_PAY_RELAY_ACTIVE_LOW ? 0 : 1;
    int inactive_level = QZ_PAY_RELAY_ACTIVE_LOW ? 1 : 0;

    ESP_LOGI(TAG, "Triggering relay (GPIO%d) for %d ms", QZ_PAY_RELAY_PIN, duration_ms);

    gpio_set_level(QZ_PAY_RELAY_PIN, active_level);
    s_request.relay_trigger_time = esp_timer_get_time() / 1000;

    /* Non-blocking: we set the relay on, main loop will turn it off */
    /* For simplicity, use a short blocking delay */
    vTaskDelay(pdMS_TO_TICKS(duration_ms));

    gpio_set_level(QZ_PAY_RELAY_PIN, inactive_level);

    ESP_LOGI(TAG, "Relay released");
#endif
}

void quartz_pay_cancel(void) {
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
