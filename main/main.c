/**
 * main.c — Quartz ESP32-S3 Miner (Headless Build)
 *
 * Minimal main for first flashable binary.
 * Mining loop + WiFi connection to testnet node.
 * Display/button support added per-board in full build.
 */

#include "quartz.h"
#include "quartz_attest.h"
#include "quartz_pool.h"
#include "quartz_entropy.h"
#include "quartz_supply_chain.h"
#include "quartz_wallet.h"
#include "quartz_display.h"
#include "quartz_wifi.h"
#include "quartz_puf.h"
#include "quartz_pay.h"
#include "quartz_agent.h"
#include "quartz_ble.h"
#include "quartz_mesh.h"
#include "quartz_qr.h"
#include <string.h>
#include <stdio.h>
#include <strings.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/adc.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#else
#define ESP_LOGI(tag, fmt, ...)
#define ESP_LOGE(tag, fmt, ...)
#define ESP_LOGW(tag, fmt, ...)
#define vTaskDelay(x)
#define portTICK_PERIOD_MS 1
#endif

/* Runtime scratchpad size — set during init based on available RAM */
int g_scratchpad_size = QUARTZ_SCRATCHPAD_SIZE;

static const char *TAG = "MAIN";
uint32_t g_last_hps = 0;  /* current hashrate, read by mining_submit */

/* === Testnet Configuration === */
#define QUARTZ_NODE_HOST "167.233.19.85"
#define QUARTZ_NODE_PORT 21100

/* === Buttons ===
 * ESP32 (M5Stack Core): 3 buttons A/B/C on GPIO 39/38/37.
 * ESP32-S3 (Heltec V3): 1 USER button on GPIO0 — cycles screens.
 */
/* PIN entry state — shared by display paths on all boards */
static char s_pin_display[9] = {0};
static int s_pin_len = 0;
static int s_pin_digit = 0;
static float s_payment_amount = 0.1f;  /* default QR amount */

#ifdef CONFIG_IDF_TARGET_ESP32

/* === M5Stack Core Buttons === */
#define BTN_A_PIN   39   /* Left button (SENSOR_VN) */
#define BTN_B_PIN   38   /* Middle button */
#define BTN_C_PIN   37   /* Right button */

/* Button state */
static bool btn_a_pressed = false;
static bool btn_b_pressed = false;
static bool btn_c_pressed = false;
static uint32_t btn_last_read_sec = 0;
static uint32_t btn_debounce_count = 0;
static int64_t btn_a_low_since_us = 0;  /* timestamp when A first read low */
static int64_t btn_b_low_since_us = 0;
static int64_t btn_c_low_since_us = 0;

#define BTN_DEBOUNCE_US       200000  /* 200ms debounce */
#define BTN_COOLDOWN_SEC       1       /* min 1s between button actions */
#define BTN_STARTUP_GRACE_SEC  10      /* ignore button presses in first 10s after boot */
#define BTN_FLOAT_CHECK_COUNT  5       /* samples for floating-pin detection */
#define BTN_FLOAT_CHECK_US     1000    /* 1ms between samples */

static void init_buttons(void) {
    /* GPIO39 (BTN_A) is shared with light sensor ADC1_CH3.
     * GPIO38/37 are ADC1_CH2/CH1 but work as digital (agent doesn't
     * reconfigure them). We'll read BTN_A via ADC instead. */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_B_PIN) | (1ULL << BTN_C_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
    /* GPIO39 is configured as ADC by quartz_agent_init — no digital config needed */
}

/* Distinguish a real button press from a floating pin.
 * For GPIO38/37: digital read with multi-sample.
 * For GPIO39: ADC read (pin is in ADC mode for light sensor).
 * When button pressed → GND → ADC ~0. When unpressed → floats higher. */
static bool btn_is_pressed_digital(int pin) {
    for (int i = 0; i < BTN_FLOAT_CHECK_COUNT; i++) {
        if (gpio_get_level(pin) != 0) return false;
        ets_delay_us(BTN_FLOAT_CHECK_US);
    }
    return true;
}

static bool btn_a_is_pressed(void) {
    /* GPIO39 is ADC1_CHANNEL_3. Read ADC: 0 = pressed (GND), >200 = not pressed */
    int val = adc1_get_raw(ADC1_CHANNEL_3);
    return (val >= 0 && val < 100);  /* threshold: pressed = near 0V */
}

static void poll_buttons(void) {
    uint32_t now = esp_timer_get_time() / 1000000;

    /* Ignore buttons during startup grace period */
    if (now < BTN_STARTUP_GRACE_SEC) return;

    /* Cooldown check (applies to all buttons) */
    if (btn_last_read_sec > 0 && (now - btn_last_read_sec) < BTN_COOLDOWN_SEC) return;

    int64_t now_us = esp_timer_get_time();

    /* Read button states.
     * A: via ADC (GPIO39 is in ADC mode for light sensor)
     * B/C: via digital GPIO (work fine) */
    bool a_raw = btn_a_is_pressed();
    bool b_raw = (gpio_get_level(BTN_B_PIN) == 0);
    bool c_raw = (gpio_get_level(BTN_C_PIN) == 0);

    /* Track when each button first goes active */
    if (a_raw) { if (btn_a_low_since_us == 0) btn_a_low_since_us = now_us; }
    else { btn_a_low_since_us = 0; btn_a_pressed = false; }

    if (b_raw) { if (btn_b_low_since_us == 0) btn_b_low_since_us = now_us; }
    else { btn_b_low_since_us = 0; btn_b_pressed = false; }

    if (c_raw) { if (btn_c_low_since_us == 0) btn_c_low_since_us = now_us; }
    else { btn_c_low_since_us = 0; btn_c_pressed = false; }

    /* BTN A: toggle mining <-> payment screen, or PIN digit+ if locked */
    if (a_raw && !btn_a_pressed && btn_a_low_since_us > 0 &&
        (now_us - btn_a_low_since_us) >= BTN_DEBOUNCE_US) {
        btn_a_pressed = true;
        btn_last_read_sec = now;
        if (quartz_display_get_screen() == QZ_SCREEN_PIN_ENTRY) {
            /* PIN entry: A increments current digit */
            s_pin_digit = (s_pin_digit + 1) % 10;
            quartz_display_pin_entry_m5stack(s_pin_digit, s_pin_len,
                10 - quartz_wallet_pin_attempts());
        } else if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            quartz_display_set_screen(QZ_SCREEN_MINING);
        } else {
            quartz_display_set_screen(QZ_SCREEN_PAYMENT);
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        }
        return;
    }

    /* BTN B: payment amount+, or PIN next digit if locked */
    if (b_raw && !btn_b_pressed && btn_b_low_since_us > 0 &&
        (now_us - btn_b_low_since_us) >= BTN_DEBOUNCE_US) {
        if (!btn_is_pressed_digital(BTN_B_PIN)) {
            btn_b_low_since_us = 0;
            return;
        }
        btn_b_pressed = true;
        btn_last_read_sec = now;
        if (quartz_display_get_screen() == QZ_SCREEN_PIN_ENTRY) {
            /* PIN entry: B locks current digit and moves to next */
            if (s_pin_len < 8) {
                s_pin_display[s_pin_len++] = '0' + s_pin_digit;
                s_pin_display[s_pin_len] = '\0';
                s_pin_digit = 0;
            }
            quartz_display_pin_entry_m5stack(s_pin_digit, s_pin_len,
                10 - quartz_wallet_pin_attempts());
        } else if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            s_payment_amount += 0.1f;
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        } else {
            /* Rotate: ID → FLEET → MINING → ID */
            qz_screen_t cur = quartz_display_get_screen();
            if (cur == QZ_SCREEN_ID) {
                quartz_display_set_screen(QZ_SCREEN_FLEET);
            } else if (cur == QZ_SCREEN_FLEET) {
                quartz_display_set_screen(QZ_SCREEN_MINING);
            } else if (cur == QZ_SCREEN_MINING) {
                quartz_display_set_screen(QZ_SCREEN_ID);
            }
        }
        return;
    }

    /* BTN C: payment amount-, or PIN confirm if locked */
    if (c_raw && !btn_c_pressed && btn_c_low_since_us > 0 &&
        (now_us - btn_c_low_since_us) >= BTN_DEBOUNCE_US) {
        if (!btn_is_pressed_digital(BTN_C_PIN)) {
            btn_c_low_since_us = 0;
            return;
        }
        btn_c_pressed = true;
        btn_last_read_sec = now;
        if (quartz_display_get_screen() == QZ_SCREEN_PIN_ENTRY) {
            /* PIN entry: C confirms and submits PIN */
            if (s_pin_len >= 4) {
                if (quartz_wallet_check_pin(s_pin_display) == QZ_WALLET_OK) {
                    quartz_wallet_reset_pin_attempts();
                    ESP_LOGI(TAG, "PIN correct via M5Stack buttons");
                    quartz_display_clear(QZ_COLOR_BLACK);
                    quartz_display_set_screen(QZ_SCREEN_ID);
                } else {
                    ESP_LOGW(TAG, "Wrong PIN via buttons");
                    quartz_wallet_record_failed_pin();
                    s_pin_len = 0;
                    s_pin_display[0] = '\0';
                    s_pin_digit = 0;
                    quartz_display_pin_entry_m5stack(0, 0,
                        10 - quartz_wallet_pin_attempts());
                }
            }
        } else if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            if (s_payment_amount > 0.1f) s_payment_amount -= 0.1f;
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        }
        return;
    }
}

#else /* CONFIG_IDF_TARGET_ESP32S3 — Heltec V3 USER button */

#define S3_BTN_PIN             0    /* USER / BOOT button */
#define S3_BTN_DEBOUNCE_US     300000
#define S3_BTN_COOLDOWN_SEC     1
#define S3_BTN_STARTUP_GRACE    10

static int64_t s3_btn_low_since_us = 0;
static bool s3_btn_latched = false;
static uint32_t s3_btn_last_sec = 0;

static void init_buttons(void) {
    gpio_config_t conf = {
        .pin_bit_mask = 1ULL << S3_BTN_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&conf);
}

/* USER button: cycle ID → MINING → FLEET → PAYMENT → ID.
 * Long-press (>1.5s) on PAYMENT bumps the QR amount. */
static void poll_buttons(void) {
    uint32_t now = esp_timer_get_time() / 1000000;
    if (now < S3_BTN_STARTUP_GRACE) return;
    if (s3_btn_last_sec && (now - s3_btn_last_sec) < S3_BTN_COOLDOWN_SEC) return;

    int64_t now_us = esp_timer_get_time();
    bool raw = (gpio_get_level(S3_BTN_PIN) == 0);

    if (raw) {
        if (s3_btn_low_since_us == 0) s3_btn_low_since_us = now_us;
        if (!s3_btn_latched && (now_us - s3_btn_low_since_us) >= S3_BTN_DEBOUNCE_US) {
            s3_btn_latched = true;
            s3_btn_last_sec = now;

            qz_screen_t cur = quartz_display_get_screen();
            switch (cur) {
            case QZ_SCREEN_ID:
                quartz_display_set_screen(QZ_SCREEN_MINING);
                break;
            case QZ_SCREEN_MINING:
                quartz_display_set_screen(QZ_SCREEN_FLEET);
                break;
            case QZ_SCREEN_FLEET:
                quartz_display_set_screen(QZ_SCREEN_PAYMENT);
                quartz_display_qr_payment(quartz_wallet_get_address(),
                                          s_payment_amount);
                break;
            case QZ_SCREEN_PAYMENT:
            default:
                quartz_display_set_screen(QZ_SCREEN_ID);
                break;
            }
        }
    } else {
        s3_btn_low_since_us = 0;
        s3_btn_latched = false;
    }
}

#endif /* target buttons */

/* === Persistent serial commands (available while mining) ===
 * Post-setup the old first-boot command loop never ran — 'setpin' was
 * unreachable on an existing wallet. This fixes that. */
static char s_cmd_buf[256];
static int  s_cmd_pos = 0;
static bool s_serial_unlocked = false;

static void quartz_serial_command(const char *cmd)
{
    if (strncasecmp(cmd, "setpin ", 7) == 0) {
        const char *pin = cmd + 7;
        if (quartz_wallet_has_pin() && !s_serial_unlocked) {
            ESP_LOGE(TAG, "PIN already set — unlock first: 'pin <current PIN>'");
            return;
        }
        if (strlen(pin) < 4 || strlen(pin) > 8) {
            ESP_LOGE(TAG, "PIN must be 4-8 digits");
            return;
        }
        quartz_wallet_set_pin(pin);
        s_serial_unlocked = true;
        ESP_LOGI(TAG, "✅ PIN set");
    } else if (strncasecmp(cmd, "pin ", 4) == 0) {
        const char *pin = cmd + 4;
        if (quartz_wallet_check_pin(pin) == QZ_WALLET_OK) {
            quartz_wallet_reset_pin_attempts();
            s_serial_unlocked = true;
            ESP_LOGI(TAG, "✅ Unlocked (serial session)");
        } else {
            ESP_LOGW(TAG, "❌ Wrong PIN (%d/10 lifetime attempts used)",
                     quartz_wallet_pin_attempts());
            if (quartz_wallet_record_failed_pin()) {
                ESP_LOGE(TAG, "🚨 MAX ATTEMPTS — WALLET WIPED");
            }
        }
    } else if (strcasecmp(cmd, "pinstatus") == 0) {
        ESP_LOGI(TAG, "PIN: %s | failed attempts: %d/10 | serial: %s",
                 quartz_wallet_has_pin() ? "SET" : "NONE",
                 quartz_wallet_pin_attempts(),
                 s_serial_unlocked ? "unlocked" : "locked");
    } else if (strcasecmp(cmd, "seed") == 0) {
        if (quartz_wallet_has_pin() && !s_serial_unlocked) {
            ESP_LOGE(TAG, "Locked — unlock first: 'pin <digits>'");
            return;
        }
        char words[12][12];
        if (quartz_wallet_get_seed_phrase_for_backup(words, 12) == QZ_WALLET_OK) {
            ESP_LOGW(TAG, "=== SEED PHRASE — write on paper, never type it in ===");
            for (int i = 0; i < 12; i++) {
                ESP_LOGI(TAG, "%2d. %s", i + 1, words[i]);
            }
            quartz_wallet_wipe_seed_phrase(words);
            if (!quartz_wallet_has_pin()) {
                ESP_LOGW(TAG, "⚠ No PIN set — protect this: 'setpin <4-8 digits>'");
            }
        } else {
            ESP_LOGE(TAG, "No wallet on device");
        }
    } else if (strcasecmp(cmd, "address") == 0) {
        const char *addr = quartz_wallet_get_address();
        if (addr && addr[0]) {
            ESP_LOGI(TAG, "Address: %s", addr);
        } else {
            ESP_LOGE(TAG, "No wallet on device");
        }
    } else if (strncasecmp(cmd, "recover ", 8) == 0) {
        /* recover word1 word2 ... word12 — adopt a phone/other wallet */
        if (quartz_wallet_has_pin() && !s_serial_unlocked) {
            ESP_LOGE(TAG, "Locked — unlock first: 'pin <digits>'");
            return;
        }
        char words[12][12];
        int wi = 0;
        const char *tok = cmd + 8;
        bool ok = true;
        while (wi < 12) {
            while (*tok == ' ') tok++;
            if (*tok == '\0') { ok = false; break; }
            const char *end = strchr(tok, ' ');
            size_t len = end ? (size_t)(end - tok) : strlen(tok);
            if (len == 0 || len > 11) { ok = false; break; }
            memcpy(words[wi], tok, len);
            words[wi][len] = '\0';
            for (char *p = words[wi]; *p; p++) *p = tolower((unsigned char)*p);
            wi++;
            tok = end ? end + 1 : tok + len;
        }
        if (ok && *tok == ' ') { while (*tok == ' ') tok++; }
        if (!ok || wi != 12 || *tok != '\0') {
            ESP_LOGE(TAG, "Usage: recover <12 words>  (exactly 12 words)");
            return;
        }
        bool tn = quartz_wallet_is_testnet();
        quartz_wallet_err_t rerr = quartz_wallet_restore(words, tn);
        if (rerr == QZ_WALLET_OK) {
            ESP_LOGI(TAG, "✅ Wallet restored — address: %s", quartz_wallet_get_address());
            ESP_LOGI(TAG, "Rebooting in 2s ...");
            vTaskDelay(pdMS_TO_TICKS(2000));
            esp_restart();
        } else if (rerr == QZ_WALLET_ERR_INVALID) {
            ESP_LOGE(TAG, "❌ Invalid seed phrase (unknown word or bad checksum)");
        } else {
            ESP_LOGE(TAG, "❌ Restore failed (code %d)", rerr);
        }
    } else if (strncasecmp(cmd, "send ", 5) == 0) {
        /* send <address> <amount_qz> — signs on-device, broadcasts via node */
        if (quartz_wallet_has_pin() && !s_serial_unlocked) {
            ESP_LOGE(TAG, "Locked — unlock first: 'pin <digits>'");
            return;
        }
        const char *p = cmd + 5;
        while (*p == ' ') p++;
        const char *sp = strchr(p, ' ');
        if (!sp) { ESP_LOGE(TAG, "Usage: send <address> <amount_qz>  (e.g. send QkAbc... 1.5)"); return; }
        char to[40];
        size_t alen = (size_t)(sp - p);
        if (alen == 0 || alen >= sizeof(to)) { ESP_LOGE(TAG, "Bad address"); return; }
        memcpy(to, p, alen);
        to[alen] = '\0';
        const char *amt = sp + 1;
        while (*amt == ' ') amt++;

        long long sats = 0;
        int frac = 0;
        bool dot = false, ok = true;
        for (const char *q = amt; *q && *q != ' '; q++) {
            if (*q == '.') {
                if (dot) { ok = false; break; }
                dot = true;
                continue;
            }
            if (*q < '0' || *q > '9') { ok = false; break; }
            if (dot) {
                if (frac >= 8) { ok = false; break; }
                frac++;
            }
            sats = sats * 10 + (*q - '0');
        }
        for (int i = frac; i < 8; i++) sats *= 10;
        if (!ok || sats <= 0) { ESP_LOGE(TAG, "Bad amount (max 8 decimals)"); return; }

        const char *from = quartz_wallet_get_address();
        const uint8_t *pub = quartz_wallet_get_pubkey();
        if (!from || !pub) { ESP_LOGE(TAG, "No wallet on device"); return; }

        char sats_str[24], msg[128], msg_hex[241], sig_hex[129], pub_hex[65], amount_qz[32];
        snprintf(sats_str, sizeof(sats_str), "%lld", sats);
        snprintf(msg, sizeof(msg), "%s%s%s", from, to, sats_str);

        uint8_t sig[64];
        if (quartz_wallet_sign((const uint8_t *)msg, strlen(msg), sig) != QZ_WALLET_OK) {
            ESP_LOGE(TAG, "Signing failed");
            return;
        }
        static const char hx[] = "0123456789abcdef";
        for (int i = 0; i < 64; i++) {
            sig_hex[i * 2] = hx[sig[i] >> 4];
            sig_hex[i * 2 + 1] = hx[sig[i] & 0xF];
        }
        sig_hex[128] = '\0';
        for (int i = 0; i < 32; i++) {
            pub_hex[i * 2] = hx[pub[i] >> 4];
            pub_hex[i * 2 + 1] = hx[pub[i] & 0xF];
        }
        pub_hex[64] = '\0';
        for (size_t i = 0; i < strlen(msg); i++) {
            msg_hex[i * 2] = hx[(uint8_t)msg[i] >> 4];
            msg_hex[i * 2 + 1] = hx[(uint8_t)msg[i] & 0xF];
        }
        msg_hex[strlen(msg) * 2] = '\0';

        long long whole = sats / 100000000LL, rem = sats % 100000000LL;
        snprintf(amount_qz, sizeof(amount_qz), "%lld.%08lld", whole, rem);
        for (char *e = amount_qz + strlen(amount_qz) - 1; *e == '0'; e--) *e = '\0';
        if (amount_qz[strlen(amount_qz) - 1] == '.') amount_qz[strlen(amount_qz) - 1] = '\0';

        char body[768], response[1024];
        snprintf(body, sizeof(body),
                 "{\"from\":\"%s\",\"to\":\"%s\",\"amount\":%s,"
                 "\"signature\":\"%s\",\"public_key\":\"%s\",\"message\":\"%s\"}",
                 from, to, amount_qz, sig_hex, pub_hex, msg_hex);

        ESP_LOGI(TAG, "Sending %s QZ to %s ...", amount_qz, to);
        int rc = quartz_http_request("POST", "/api/v1/send", body, response, sizeof(response));
        if (rc < 0) {
            ESP_LOGE(TAG, "Node unreachable (rc=%d) — WiFi connected?", rc);
        } else {
            ESP_LOGI(TAG, "Node: %s", response);
            if (strstr(response, "\"txid\"")) {
                ESP_LOGI(TAG, "✅ Sent — pending in mempool, mined within ~30s");
            }
        }
    } else if (strcasecmp(cmd, "help") == 0) {
        ESP_LOGI(TAG, "Commands:");
        ESP_LOGI(TAG, "  address              show wallet address");
        ESP_LOGI(TAG, "  seed                 show backup phrase");
        ESP_LOGI(TAG, "  recover <12 words>   import wallet from seed phrase, reboot");
        ESP_LOGI(TAG, "  send <addr> <amount> sign + broadcast tx (e.g. send Qk... 1.5)");
        ESP_LOGI(TAG, "  setpin/pin <digits>  set or unlock PIN");
        ESP_LOGI(TAG, "  pinstatus            PIN state");
    }
}

static void quartz_serial_poll(void)
{
    char ch;
    while (read(STDIN_FILENO, &ch, 1) == 1) {
        if (ch == '\n' || ch == '\r') {
            /* Echo newline so the command is visually complete */
            char nl = '\n';
            write(STDOUT_FILENO, &nl, 1);
            if (s_cmd_pos > 0) {
                s_cmd_buf[s_cmd_pos] = '\0';
                quartz_serial_command(s_cmd_buf);
                s_cmd_pos = 0;
            }
        } else if (ch == 0x7f || ch == 0x08) {
            /* Backspace — echo erase */
            if (s_cmd_pos > 0) {
                s_cmd_pos--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
        } else if (s_cmd_pos < (int)sizeof(s_cmd_buf) - 1) {
            /* Echo character as typed (works in any terminal, zero config) */
            write(STDOUT_FILENO, &ch, 1);
            s_cmd_buf[s_cmd_pos++] = ch;
        }
    }
}

/* === Mining State === */
static volatile bool s_mining = false;
/* mining stats now in quartz.c */
static uint32_t s_start_time = 0;
static uint32_t s_hash_count = 0;
static uint32_t s_blocks_found = 0;

#ifdef ESP_PLATFORM
/* === NVS Init === */
static void init_nvs(void) {
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
}

/* === Mining Task === */
static void mining_task(void *pvParameters) {
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "  Quartz (QZ) — ESP32 Cryptocurrency Miner");
    ESP_LOGI(TAG, "  Firmware: %s", FW_VERSION_STRING);
    ESP_LOGI(TAG, "  Protocol: %d", QUARTZ_VERSION);
    ESP_LOGI(TAG, "  Target: ESP32-S3 (generic)");
    ESP_LOGI(TAG, "  Node: %s:%d", QUARTZ_NODE_HOST, QUARTZ_NODE_PORT);
    ESP_LOGI(TAG, "========================================");

    /* Initialize entropy subsystem */
    ESP_LOGI(TAG, "Waiting for hardware RNG...");
    qz_err_t err = quartz_entropy_wait_for_ready();
    if (err != QZ_OK) {
        ESP_LOGE(TAG, "RNG not ready — cannot mine safely");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "Hardware RNG ready (%.2f bits/byte entropy)",
             quartz_entropy_last_estimate());

    /* Initialize supply chain (birth certificate) */
    qz_birth_certificate_t cert;
    err = quartz_supply_chain_init(&cert);
    if (err != QZ_OK) {
        ESP_LOGE(TAG, "Supply chain init failed (code %d)", err);
        ESP_LOGE(TAG, "Device may be tampered — refusing to mine");
        vTaskDelete(NULL);
        return;
    }

    /* Show device identity */
    char short_hash[9];
    quartz_cert_short_hash(&cert, short_hash);
    ESP_LOGI(TAG, "Device: %02x:%02x:%02x:%02x:%02x:%02x  Key: %s",
             cert.chip_id[0], cert.chip_id[1], cert.chip_id[2],
             cert.chip_id[3], cert.chip_id[4], cert.chip_id[5],
             short_hash);

    /* Initialize wallet */
    quartz_wallet_err_t werr = quartz_wallet_load();
    bool seed_needs_display = false;

    if (werr == QZ_WALLET_ERR_NOT_FOUND) {
        ESP_LOGI(TAG, "========================================");
        ESP_LOGI(TAG, "  Creating new Quartz wallet...");
        ESP_LOGI(TAG, "========================================");

        werr = quartz_wallet_generate(false);  /* mainnet */
        if (werr != QZ_WALLET_OK) {
            ESP_LOGE(TAG, "Wallet generation failed (code %d)", werr);
            vTaskDelete(NULL);
            return;
        }
        seed_needs_display = true;
    } else if (werr == QZ_WALLET_OK) {
        /* Wallet exists — check if seed was ever confirmed */
        if (!quartz_wallet_is_backup_confirmed()) {
            ESP_LOGW(TAG, "⚠️ Wallet exists but seed phrase was NEVER confirmed!");
            ESP_LOGW(TAG, "⚠️ Re-displaying seed for backup...");
            seed_needs_display = true;
        } else {
            ESP_LOGI(TAG, "Wallet loaded: %s", quartz_wallet_get_address());
        }
    } else {
        ESP_LOGE(TAG, "Wallet load failed (code %d)", werr);
        quartz_display_error("Wallet load failed");
    }

    if (seed_needs_display) {
        /* Gate seed display behind PIN if set */
        if (quartz_wallet_has_pin() && !quartz_ble_is_unlocked()) {
            ESP_LOGI(TAG, "PIN set — seed locked. Enter via serial 'pin <digits>' or BLE app");
#ifdef QUARTZ_HAS_DISPLAY
            quartz_display_clear(QZ_COLOR_BLACK);
            quartz_display_draw_text(40, 100, "PIN Required", QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
            quartz_display_draw_text(20, 130, "Enter via app or serial", QZ_COLOR_GRAY, QZ_COLOR_BLACK);
#endif
            while (!quartz_ble_is_unlocked()) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
        }

        char words[12][12];
        werr = quartz_wallet_get_seed_phrase_for_backup(words, 12);
        if (werr == QZ_WALLET_OK) {
            /* Build seed QR payload: compact BIP-39 format (space-separated words)
             * No JSON wrapper — keeps payload under 100 chars so it fits even
             * at QR_ECC_HIGH (v10 max=119). Address is derivable from seed. */
            char qr_payload[200];
            int qlen = snprintf(qr_payload, sizeof(qr_payload), "quartz-seed:");
            for (int i = 0; i < 12; i++) {
                qlen += snprintf(qr_payload + qlen, sizeof(qr_payload) - qlen,
                    "%s%s", words[i], (i < 11) ? " " : "");
            }

            /* === SECURE CHANNELS ONLY === */
            /* NO captive portal — seed never goes over WiFi */
            /* NO unbonded BLE — seed char only readable after pairing */

            /* === SEED BACKUP — Serial is the primary channel === */
            /* On first boot, seed words are shown on serial output until confirmed.
             * Display shows a simple prompt. QR/BLE remain available but secondary. */

            /* Channel 1: Serial output (PRIMARY) — plain text words */
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔════════════════════════════════════════════╗");
            ESP_LOGI(TAG, "║   🔮 QUARTZ WALLET — SEED PHRASE           ║");
            ESP_LOGI(TAG, "║   WRITE DOWN THESE 12 WORDS                ║");
            ESP_LOGI(TAG, "╚════════════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Address: %s", quartz_wallet_get_address());
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "  ┌─────────┬─────────────┐");
            ESP_LOGI(TAG, "  │  Word   │   Value     │");
            ESP_LOGI(TAG, "  ├─────────┼─────────────┤");
            for (int i = 0; i < 12; i++) {
                ESP_LOGI(TAG, "  │  %2d     │  %-10s │", i + 1, words[i]);
            }
            ESP_LOGI(TAG, "  └─────────┴─────────────┘");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "⚠️  Anyone with these words controls your wallet.");
            ESP_LOGI(TAG, "⚠️  Write them down on paper. Do not screenshot.");
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Type 'confirm' + Enter after writing down your seed.");
            ESP_LOGI(TAG, "This message will repeat on every boot until confirmed.");
            ESP_LOGI(TAG, "");

            /* Also output as QR on serial for those who want it */
            ESP_LOGI(TAG, "── QR code (optional) ──");
            quartz_qr_serial(qr_payload, QR_ECC_HIGH);
            ESP_LOGI(TAG, "");

            /* Channel 2: Display — simple prompt, check serial for words (WIP: QR display) */
#ifdef QUARTZ_HAS_DISPLAY
            quartz_display_clear(QZ_COLOR_BLACK);
            quartz_display_draw_text(40, 60, "🔮 QUARTZ WALLET", QZ_COLOR_PURPLE, QZ_COLOR_BLACK);
            quartz_display_draw_text(20, 100, "Seed phrase on serial!", QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
            quartz_display_draw_text(10, 130, "Open serial monitor", 0xAAAAAA, QZ_COLOR_BLACK);
            quartz_display_draw_text(20, 150, "to see your 12 words", 0xAAAAAA, QZ_COLOR_BLACK);
            quartz_display_draw_text(10, 190, "Type 'confirm' + Enter", 0x00FF00, QZ_COLOR_BLACK);
            quartz_display_draw_text(30, 210, "after writing down", 0x00FF00, QZ_COLOR_BLACK);
#endif

            /* Channel 3: BLE (bonded only — app must pair first) */
            /* Seed characteristic returns empty unless bonded */
            quartz_ble_set_seed_phrase((const char (*)[12])words);

            ESP_LOGI(TAG, "Waiting for confirmation...");
            ESP_LOGI(TAG, "  - HOLD BOOT/PRG BUTTON 3s (no PC needed)");
            ESP_LOGI(TAG, "  - Quartz app (BLE): pair device, then confirm in app");
            ESP_LOGI(TAG, "  - Serial: type 'confirm' + Enter");

            /* Serial confirmation input state */
            char serial_buf[32] = {0};
            int serial_pos = 0;
            bool serial_confirmed = false;

            /* BOOT/PRG button (GPIO0, active-low) — hold 3s to confirm.
             * GPIO0 = PRG button on LilyGO T3; harmless on M5Stack (speaker SD).
             * Same rationale as v069-c3: host serial tx is often wedged. */
            gpio_config_t boot_btn = {
                .pin_bit_mask = 1ULL << 0,
                .mode = GPIO_MODE_INPUT,
                .pull_up_en = GPIO_PULLUP_ENABLE,
            };
            gpio_config(&boot_btn);
            int boot_hold_ms = 0;

            /* Wait for confirmation from ANY source — NO TIMEOUT */
            while (!quartz_ble_is_seed_confirmed() &&
                   !serial_confirmed) {
                /* Check serial input via non-blocking read */
                char ch;
                int n = read(STDIN_FILENO, &ch, 1);
                if (n == 1) {
                    if (ch == '\n' || ch == '\r') {
                        /* Echo newline */
                        char nl = '\n';
                        write(STDOUT_FILENO, &nl, 1);
                        serial_buf[serial_pos] = '\0';
                        /* Serial commands */
                        if (strcasecmp(serial_buf, "confirm") == 0) {
                            serial_confirmed = true;
                            ESP_LOGI(TAG, "✅ Seed confirmed via serial input");
                        } else if (strncasecmp(serial_buf, "pin ", 4) == 0) {
                            /* Unlock with PIN */
                            const char *pin = serial_buf + 4;
                            if (quartz_wallet_check_pin(pin) == QZ_WALLET_OK) {
                                quartz_wallet_reset_pin_attempts();
                                ESP_LOGI(TAG, "✅ PIN correct — device unlocked");
                            } else {
                                ESP_LOGW(TAG, "❌ Wrong PIN (attempt %d/10)",
                                         quartz_wallet_pin_attempts() + 1);
                                quartz_wallet_record_failed_pin();
                            }
                        } else if (strncasecmp(serial_buf, "setpin ", 7) == 0) {
                            /* Set PIN */
                            const char *pin = serial_buf + 7;
                            quartz_wallet_set_pin(pin);
                        } else if (strcasecmp(serial_buf, "pinstatus") == 0) {
                            ESP_LOGI(TAG, "PIN: %s, attempts: %d/10, unlocked: %s",
                                     quartz_wallet_has_pin() ? "SET" : "NONE",
                                     quartz_wallet_pin_attempts(),
                                     quartz_ble_is_unlocked() ? "YES" : "NO");
                        } else if (strncasecmp(serial_buf, "recover ", 9) == 0) {
                            /* Recovery: 'recover word1 word2 ... word12' */
                            ESP_LOGI(TAG, "Recovery mode — parsing seed phrase...");
                            /* TODO: parse 12 words, derive address, query node */
                            ESP_LOGI(TAG, "Recovery not yet fully implemented — use app");
                        } else if (strcasecmp(serial_buf, "wifi") == 0) {
                            /* Wipe WiFi credentials and reboot into provisioning portal */
                            nvs_handle_t h;
                            if (nvs_open("qz_wifi", NVS_READWRITE, &h) == ESP_OK) {
                                nvs_erase_key(h, "ssid");
                                nvs_erase_key(h, "pass");
                                nvs_commit(h);
                                nvs_close(h);
                            }
                            ESP_LOGI(TAG, "WiFi cleared — rebooting into portal (Quartz-XXXX AP)");
                            vTaskDelay(pdMS_TO_TICKS(500));
                            esp_restart();
                        } else if (strcasecmp(serial_buf, "help") == 0) {
                            ESP_LOGI(TAG, "Commands: confirm | pin <digits> | setpin <digits> | pinstatus | recover <12 words> | wifi | help");
                        }
                        serial_pos = 0;
                        serial_buf[0] = '\0';
                    } else if (ch == 0x7f || ch == 0x08) {
                        /* Backspace */
                        if (serial_pos > 0) {
                            serial_pos--;
                            write(STDOUT_FILENO, "\b \b", 3);
                        }
                    } else if (serial_pos < (int)sizeof(serial_buf) - 1) {
                        /* Echo character as typed */
                        write(STDOUT_FILENO, &ch, 1);
                        serial_buf[serial_pos++] = ch;
                    }
                }

                /* BOOT/PRG button hold-to-confirm */
                if (gpio_get_level(0) == 0) {
                    boot_hold_ms += 50;
                    if (boot_hold_ms == 1000) {
                        ESP_LOGI(TAG, "BOOT held 1s... keep holding to confirm (3s)");
                    }
                    if (boot_hold_ms >= 3000) {
                        serial_confirmed = true;
                        ESP_LOGI(TAG, "✅ Seed confirmed via BOOT button");
                    }
                } else {
                    boot_hold_ms = 0;
                }

                vTaskDelay(pdMS_TO_TICKS(50));
            }

            /* Confirm in NVS so we never show seed again */
            quartz_wallet_confirm_backup();

            if (quartz_ble_is_seed_confirmed()) {
                ESP_LOGI(TAG, "✅ Seed confirmed via BLE app");
            } else if (serial_confirmed) {
                ESP_LOGI(TAG, "✅ Seed confirmed via serial");
            }

            /* Wipe seed phrase from RAM */
            quartz_wallet_wipe_seed_phrase(words);
#ifdef QUARTZ_HAS_DISPLAY
            quartz_display_clear(QZ_COLOR_BLACK);
#endif
        }
    }

    /* Initialize PUF (hardware binding) — NO FALLBACK.
     * If PUF fails, device refuses to mine. Period. */
    int puf_rc = quartz_puf_init();
    if (puf_rc != 0) {
        ESP_LOGE(TAG, "PUF initialization FAILED — refusing to mine.");
        ESP_LOGE(TAG, "Device halting. Power-cycle to retry (unplug, wait 5s, replug).");
        /* Display error on screen if available */
        quartz_display_clear(0x0843);
        quartz_display_fill_rect(0, 0, 320, 22, 0xF800);  /* red header */
        quartz_display_draw_text(8, 3, "PUF FAILED - HALTED", 0xFFFF, 0xF800);
        quartz_display_draw_text(8, 40, "Hardware binding required.", 0xFFFF, 0x0843);
        quartz_display_draw_text(8, 60, "No fallback key.", 0xF800, 0x0843);
        quartz_display_draw_text(8, 90, "Power-cycle to retry:", 0x8888, 0x0843);
        quartz_display_draw_text(8, 110, "1. Unplug USB", 0xFFFF, 0x0843);
        quartz_display_draw_text(8, 130, "2. Wait 5 seconds", 0xFFFF, 0x0843);
        quartz_display_draw_text(8, 150, "3. Replug", 0xFFFF, 0x0843);
        /* Infinite loop — device is bricked until power-cycle */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    ESP_LOGI(TAG, "PUF fingerprint: %s", quartz_puf_get_fingerprint());

    /* Initialize pool (solo mode by default) */
    quartz_pool_init();
    ESP_LOGI(TAG, "Mining mode: SOLO");

    /* Initialize autonomous agent */
    quartz_agent_init(quartz_wallet_get_address());
    ESP_LOGI(TAG, "Device agent ready — autonomous rules active");

    /* Allocate scratchpad — try PSRAM first, fall back to internal RAM */
    ESP_LOGI(TAG, "Allocating scratchpad...");
    uint8_t *scratchpad = heap_caps_malloc(QUARTZ_SCRATCHPAD_SIZE, MALLOC_CAP_SPIRAM);
    int scratchpad_size = QUARTZ_SCRATCHPAD_SIZE;
    if (!scratchpad) {
        ESP_LOGW(TAG, "No PSRAM — using 32KB internal RAM scratchpad");
        scratchpad = heap_caps_malloc(QUARTZ_SCRATCHPAD_SIZE_LITE, MALLOC_CAP_8BIT);
        scratchpad_size = QUARTZ_SCRATCHPAD_SIZE_LITE;
    }
    if (!scratchpad) {
        ESP_LOGE(TAG, "Failed to allocate scratchpad — out of memory");
        vTaskDelete(NULL);
        return;
    }
    g_scratchpad_size = scratchpad_size;
    ESP_LOGI(TAG, "Scratchpad allocated (%d KB)", scratchpad_size / 1024);

    /* BLE only while seed provisioning might still be needed (v070).
     * Once backup is confirmed, dedicate the radio to WiFi and kill
     * modem-sleep coex churn (router evictions). */
    bool ble_on = !quartz_wallet_is_backup_confirmed();
    if (ble_on) {
        quartz_ble_set_address(quartz_wallet_get_address());
        quartz_ble_init();
        ESP_LOGI(TAG, "BLE ready — pair as \"Quartz-Miner\"");
    } else {
        ESP_LOGI(TAG, "BLE off (seed confirmed) — radio dedicated to WiFi");
        quartz_wifi_set_full_power();
    }

    /* If PIN is set, show PIN entry screen before mining starts */
    if (quartz_wallet_has_pin()) {
        ESP_LOGI(TAG, "PIN set — device locked until PIN entered");
        ESP_LOGI(TAG, "Enter PIN via serial: 'pin <digits>' or BLE app");
#ifdef QUARTZ_HAS_DISPLAY
        quartz_display_set_screen(QZ_SCREEN_PIN_ENTRY);
        s_pin_len = 0;
        s_pin_digit = 0;
        s_pin_display[0] = '\0';
        quartz_display_pin_entry_m5stack(0, 0, 10);
#endif
        /* Mining starts, but seed/signing stay locked */
        /* User can enter PIN anytime via buttons, serial, or BLE */
    }

    /* Mining loop */
    s_mining = true;
    s_start_time = esp_timer_get_time() / 1000000;
    s_hash_count = 0;

    ESP_LOGI(TAG, "Mining started! (serial: 'help' for commands — setpin/seed/pinstatus)");

    /* Draw identity screen immediately — it's the boot default */
#ifdef QUARTZ_HAS_DISPLAY
    {
        uint8_t devid[32] = {0};
        quartz_attest_get_device_id(devid);
        quartz_display_set_screen(QZ_SCREEN_ID);
        quartz_display_id_screen(quartz_attest_is_provisioned(), devid, 0, 0,
                                 quartz_wallet_get_address());
    }
#endif

    uint8_t header[80] = {0};
    uint64_t nonce = 0;
    uint8_t hash[32];

    /* Try to fetch work from node */
    qz_block_template_t tmpl;
    bool have_work = false;
    uint32_t last_work_fetch = 0;

    /* v071: offline block stash — found block awaiting submission.
     * Kept in RAM, retried every 5s once online, dropped if the chain
     * advances past its height (orphaned). No more thrown-away blocks. */
    static struct {
        bool     valid;
        char     job_id[32];
        uint64_t nonce;
        uint8_t  header[80];
        uint32_t height;
        uint32_t last_try;
    } pend = {0};

    while (s_mining) {
        quartz_serial_poll();
        /* Fetch new work every 30 seconds or on first iteration */
        uint32_t now = esp_timer_get_time() / 1000000;
        if (quartz_wifi_is_connected() && (!have_work || (now - last_work_fetch) > 30)) {
            int rc = quartz_mining_get_work(&tmpl);
            if (rc == 0) {
                memcpy(header, tmpl.header, 80);
                nonce = 0;
                have_work = true;
                last_work_fetch = now;
                ESP_LOGI(TAG, "📡 Got work: block %d, target %d",
                         tmpl.height, tmpl.target_bits);
                /* Share work with mesh peers */
                quartz_mesh_share_work(&tmpl);
            } else {
                /* Fallback to local mining if node unreachable */
                if (!have_work) {
                    ESP_LOGW(TAG, "Node unreachable, mining locally");
                }
            }
        }

        /* If no WiFi, try to get work from mesh peers */
        if (!have_work && quartz_mesh_is_active()) {
            if (quartz_mesh_get_work(&tmpl) == 0) {
                memcpy(header, tmpl.header, 80);
                nonce = 0;
                have_work = true;
                last_work_fetch = now;
                ESP_LOGI(TAG, "📡 Got work via mesh: block %d, target %d",
                         tmpl.height, tmpl.target_bits);
            }
        }

        /* v071: retry stashed block once the link is back */
        if (pend.valid) {
            if (have_work && tmpl.height > pend.height) {
                ESP_LOGW(TAG, "Stashed block #%u orphaned (chain at %u) - dropped",
                         pend.height, tmpl.height);
                pend.valid = false;
            } else if (quartz_wifi_is_connected() && (now - pend.last_try) >= 5) {
                pend.last_try = now;
                int rc = quartz_mining_submit(pend.job_id, pend.nonce, pend.header);
                if (rc == 0) {
                    ESP_LOGI(TAG, "Stashed block #%u ACCEPTED after retry (+42 QZ)", pend.height);
                    pend.valid = false;
                } else {
                    ESP_LOGW(TAG, "Stash retry failed (%d), trying again in 5s", rc);
                }
            }
        }

        crystal_hash_v2(header, nonce, hash, scratchpad, true);

        s_hash_count++;

        /* Check difficulty target (simple: top N bits zero) */
        if (have_work) {
            /* Check if hash meets target */
            uint32_t target = tmpl.target_bits;
            if (target > 0 && target <= 32) {
                uint32_t *hash_words = (uint32_t *)hash;
                /* Check if top 'target' bits are zero */
                bool meets = true;
                int bits_checked = 0;
                for (int w = 0; w < 8 && bits_checked < (int)target; w++) {
                    uint32_t word = __builtin_bswap32(hash_words[w]);
                    int bits_in_word = target - bits_checked;
                    if (bits_in_word > 32) bits_in_word = 32;
                    uint32_t mask = 0xFFFFFFFF << (32 - bits_in_word);
                    if (word & mask) { meets = false; break; }
                    bits_checked += bits_in_word;
                }
                if (meets) {
                    ESP_LOGI(TAG, "BLOCK FOUND! Nonce %llu", nonce);
                    s_blocks_found++;

                    /* Generate PUF attestation for this block */
                    uint8_t puf_resp[32];
                    quartz_puf_mining_response(header, nonce, puf_resp);
                    ESP_LOGI(TAG, "PUF attestation: %02x%02x%02x%02x...",
                             puf_resp[0], puf_resp[1], puf_resp[2], puf_resp[3]);

                    /* Share block find with mesh peers */
                    quartz_mesh_share_found(header, nonce);

                    /* Submit to node (v071: stash for retry if offline or submit fails) */
                    if (quartz_wifi_is_connected()) {
                        int rc = quartz_mining_submit(tmpl.job_id, nonce, header);
                        if (rc == 0) {
                            ESP_LOGI(TAG, "Block submitted and accepted!");
                        } else {
                            ESP_LOGW(TAG, "Submit failed (%d) - stashing block #%u", rc, tmpl.height);
                            pend.valid = true;
                            strlcpy(pend.job_id, tmpl.job_id, sizeof(pend.job_id));
                            pend.nonce = nonce;
                            memcpy(pend.header, header, 80);
                            pend.height = tmpl.height;
                            pend.last_try = now;
                        }
                    } else {
                        ESP_LOGW(TAG, "OFFLINE - stashing block #%u (+42 QZ) until link returns",
                                 tmpl.height);
                        pend.valid = true;
                        strlcpy(pend.job_id, tmpl.job_id, sizeof(pend.job_id));
                        pend.nonce = nonce;
                        memcpy(pend.header, header, 80);
                        pend.height = tmpl.height;
                        pend.last_try = now;
                    }

                    /* Fetch new work immediately */
                    have_work = false;
                }
            }
        }

        /* Update display (only on mining screen) */
        if (s_hash_count % 56 == 0) {
            uint32_t uptime = (esp_timer_get_time() / 1000000) - s_start_time;
            uint32_t hps = (uptime > 0) ? (s_hash_count / uptime) : 0;
            g_last_hps = hps;
            /* Serial log once a minute — display keeps refreshing every pass,
             * but the console no longer drowns out typed commands */
            static uint32_t s_last_log_uptime = 0;
            if (uptime - s_last_log_uptime >= 60) {
                s_last_log_uptime = uptime;
                ESP_LOGI(TAG, "Mining... %lu H/s, %lu total, uptime %luh%lum [%s]",
                         hps, s_hash_count, uptime / 3600, (uptime % 3600) / 60, FW_VERSION_STRING);
            }

#ifdef QUARTZ_HAS_DISPLAY
            qz_screen_t cur = quartz_display_get_screen();
            if (cur == QZ_SCREEN_ID) {
                uint8_t devid[32] = {0};
                quartz_attest_get_device_id(devid);
                quartz_display_id_screen(quartz_attest_is_provisioned(), devid,
                                         uptime, hps, quartz_wallet_get_address());
            } else if (cur == QZ_SCREEN_FLEET) {
                qz_pool_stats_t pst;
                quartz_pool_get_stats(&pst);
                quartz_display_fleet_screen(pst.member_count, pst.my_shares,
                                            pst.rewards_earned_qz * 1000,
                                            pst.blocks_found);
            } else if (cur == QZ_SCREEN_MINING) {
                quartz_display_mining_stats(
                    s_hash_count,
                    hps,
                    s_blocks_found,
                    uptime,
                    quartz_wallet_get_address()
                );
            }
#endif

            /* Update BLE stats for phone app */
            quartz_ble_update_stats(s_hash_count, hps, s_blocks_found, uptime);

            /* Check for mesh-found blocks from peers — relay to node */
            if (quartz_wifi_is_connected()) {
                uint8_t m_header[80];
                uint64_t m_nonce;
                if (quartz_mesh_get_found(m_header, &m_nonce) == 0) {
                    ESP_LOGI(TAG, "📡 Relaying mesh peer's block to node (nonce %llu)", m_nonce);
                    int rc = quartz_mining_submit("mesh", m_nonce, m_header);
                    if (rc == 0) {
                        ESP_LOGI(TAG, "✅ Mesh block relayed successfully!");
                    } else {
                        ESP_LOGW(TAG, "Mesh block relay failed (%d)", rc);
                    }
                }
            }

            /* Mesh maintenance */
            quartz_mesh_step(uptime);

            /* Poll for messages every 10 seconds */
            static uint32_t last_msg_check = 0;
            if (quartz_wifi_is_connected() && (uptime - last_msg_check) > 10) {
                last_msg_check = uptime;
                qz_message_t msg;
                if (quartz_messages_get_latest(&msg) == 0) {
                    ESP_LOGI(TAG, "MAIL from %s: %s", msg.from, msg.text);
#ifdef QUARTZ_HAS_DISPLAY
                    if (quartz_display_get_screen() == QZ_SCREEN_MESSAGES) {
                        quartz_display_message(msg.from, msg.text, msg.block_height);
                    }
#endif
                }
            }

            /* Run autonomous agent rules */
            quartz_agent_step(1);
        }

        nonce++;

#ifdef QUARTZ_HAS_DISPLAY
        /* Poll buttons every iteration */
        poll_buttons();

        /* If on payment screen, don't redraw mining stats */
        if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            /* QR stays on screen — just yield to other tasks */
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
#endif

        /* Yield to other tasks (BLE, WiFi, LoRa) */
        if (s_hash_count % 100 == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    free(scratchpad);
    vTaskDelete(NULL);
}
#endif /* ESP_PLATFORM */

/* === Main Entry Point === */
#ifdef ESP_PLATFORM
void app_main(void) {
    ESP_LOGI(TAG, "Quartz ESP32 Miner %s starting...", FW_VERSION_STRING);

    /* Initialize NVS */
    init_nvs();

    /* Serial console: non-blocking so both the seed-confirmation loop
     * AND the mining loop can poll for commands without blocking */
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

    /* Initialize display FIRST so we can show portal/splash */
    /* Initialize display (skip on headless boards like LilyGO T3) */
#ifdef QUARTZ_HAS_DISPLAY
    quartz_display_init();
    quartz_display_splash();
    init_buttons();
#else
    ESP_LOGI(TAG, "Headless mode — no display/buttons");
#endif

    /* Initialize WiFi (provisioning or connect) */
    quartz_wifi_init();

    /* Wait for radio to warm up */
    vTaskDelay(pdMS_TO_TICKS(2000));

    /* If in portal mode, show instructions on display */
    if (g_wifi_state == QZ_WIFI_PORTAL_ACTIVE) {
        uint8_t mac[6];
        esp_wifi_get_mac(WIFI_IF_AP, mac);
        char ap_name[32];
        snprintf(ap_name, sizeof(ap_name), "Quartz-%02X%02X", mac[4], mac[5]);
#ifdef QUARTZ_HAS_DISPLAY
        quartz_display_portal(ap_name);
#else
        ESP_LOGI(TAG, "WiFi Portal: Connect to \"%s\", open 192.168.4.1", ap_name);
#endif
    }

    /* Wait for WiFi (or stay in portal mode forever) */
    if (g_wifi_state != QZ_WIFI_PORTAL_ACTIVE) {
#ifdef QUARTZ_HAS_DISPLAY
        quartz_display_connecting();
#endif
        quartz_wifi_wait_connected(15000);
    }

    /* Initialize ESP-NOW mesh (after WiFi is up) */
    if (quartz_wifi_is_connected()) {
        quartz_mesh_init();
        quartz_mesh_update_caps(QZ_CAP_HAS_WIFI | QZ_CAP_IS_MINING);
    } else {
        /* Still init mesh — we can receive work from peers without WiFi */
        quartz_mesh_init();
        quartz_mesh_update_caps(QZ_CAP_IS_MINING);
    }

    /* Start mining task on Core 1 (Core 0 handles WiFi/BLE/display) */
    xTaskCreatePinnedToCore(
        mining_task,
        "quartz_miner",
        32768,       /* 32KB stack */
        NULL,
        5,           /* Priority */
        NULL,
        1            /* Core 1 */
    );
}
#else
int main(void) {
    printf("This firmware must be built with ESP-IDF for ESP32-S3.\n");
    return 0;
}
#endif
