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
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
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

/* === M5Stack Core Buttons === */
#define BTN_A_PIN   39   /* Left button */
#define BTN_B_PIN   38   /* Middle button */
#define BTN_C_PIN   37   /* Right button */

/* Button state */
static bool btn_a_pressed = false;
static bool btn_b_pressed = false;
static bool btn_c_pressed = false;
static float s_payment_amount = 0.1f;  /* default QR amount */
static uint32_t btn_last_read_sec = 0;
static uint32_t btn_debounce_count = 0;

#define BTN_DEBOUNCE_NEEDED   5    /* need 5 consecutive low reads (~50ms) */
#define BTN_COOLDOWN_SEC       1    /* min 1s between button actions */

static void init_buttons(void) {
    /* M5Stack Core buttons: GPIO39, 38, 37 (input-only, no internal pull-up)
     * M5Stack PCB has external pull-ups, so unpressed = HIGH */
    gpio_config_t btn_conf = {
        .pin_bit_mask = (1ULL << BTN_A_PIN) | (1ULL << BTN_B_PIN) | (1ULL << BTN_C_PIN),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,     /* these pins don't support internal pull-up */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn_conf);
}

static void poll_buttons(void) {
    uint32_t now = esp_timer_get_time() / 1000000;

    /* Debounce: read buttons, require N consecutive same reads */
    bool a_raw = (gpio_get_level(BTN_A_PIN) == 0);
    bool b_raw = (gpio_get_level(BTN_B_PIN) == 0);
    bool c_raw = (gpio_get_level(BTN_C_PIN) == 0);

    /* If any button is raw-low, increment debounce counter */
    if (a_raw || b_raw || c_raw) {
        btn_debounce_count++;
    } else {
        btn_debounce_count = 0;
        btn_a_pressed = false;
        btn_b_pressed = false;
        btn_c_pressed = false;
        return;
    }

    /* Wait for debounce threshold */
    if (btn_debounce_count < BTN_DEBOUNCE_NEEDED) return;

    /* Cooldown check */
    if (btn_last_read_sec > 0 && (now - btn_last_read_sec) < BTN_COOLDOWN_SEC) return;

    /* BTN A: toggle mining <-> payment screen */
    if (a_raw && !btn_a_pressed) {
        btn_a_pressed = true;
        btn_last_read_sec = now;
        btn_debounce_count = 0;
        if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            quartz_display_set_screen(QZ_SCREEN_MINING);
        } else {
            quartz_display_set_screen(QZ_SCREEN_PAYMENT);
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        }
        return;
    }

    /* BTN B: increase payment amount */
    if (b_raw && !btn_b_pressed) {
        btn_b_pressed = true;
        btn_last_read_sec = now;
        btn_debounce_count = 0;
        if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            s_payment_amount += 0.1f;
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        }
        return;
    }

    /* BTN C: decrease payment amount */
    if (c_raw && !btn_c_pressed) {
        btn_c_pressed = true;
        btn_last_read_sec = now;
        btn_debounce_count = 0;
        if (quartz_display_get_screen() == QZ_SCREEN_PAYMENT) {
            if (s_payment_amount > 0.1f) s_payment_amount -= 0.1f;
            quartz_display_qr_payment(quartz_wallet_get_address(), s_payment_amount);
        }
        return;
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
    ESP_LOGI(TAG, "  Version: %d", QUARTZ_VERSION);
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

        /* Display seed phrase ONE TIME on serial output */
        char words[12][12];
        werr = quartz_wallet_get_seed_phrase_for_backup(words, 12);
        if (werr == QZ_WALLET_OK) {
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "╔══════════════════════════════════════╗");
            ESP_LOGI(TAG, "║   🔮 QUARTZ WALLET SEED PHRASE       ║");
            ESP_LOGI(TAG, "║   WRITE THIS DOWN — shown only once  ║");
            ESP_LOGI(TAG, "╚══════════════════════════════════════╝");
            ESP_LOGI(TAG, "");
            for (int i = 0; i < 12; i++) {
                ESP_LOGI(TAG, "  %2d. %s", i + 1, words[i]);
            }
            ESP_LOGI(TAG, "");
            ESP_LOGI(TAG, "Address: %s", quartz_wallet_get_address());
            ESP_LOGI(TAG, "");

            /* Display seed phrase on screen too */
            quartz_display_seed_phrase(words, 0);
            vTaskDelay(pdMS_TO_TICKS(5000));  /* Show page 1 for 5s */
            quartz_display_seed_phrase(words, 1);
            vTaskDelay(pdMS_TO_TICKS(5000));  /* Show page 2 for 5s */

            /* Wipe seed phrase from RAM */
            quartz_wallet_wipe_seed_phrase(words);
            quartz_display_clear(QZ_COLOR_BLACK);
        }
    } else if (werr != QZ_WALLET_OK) {
        ESP_LOGE(TAG, "Wallet load failed (code %d)", werr);
        quartz_display_error("Wallet load failed");
    } else {
        ESP_LOGI(TAG, "Wallet loaded: %s", quartz_wallet_get_address());
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

    /* Mining loop */
    s_mining = true;
    s_start_time = esp_timer_get_time() / 1000000;
    s_hash_count = 0;

    ESP_LOGI(TAG, "Mining started!");

    /* Draw initial mining screen immediately */
#ifdef QUARTZ_HAS_DISPLAY
    quartz_display_mining_stats(0, 0, 0, 0, quartz_wallet_get_address());
#endif

    uint8_t header[80] = {0};
    uint64_t nonce = 0;
    uint8_t hash[32];

    /* Try to fetch work from node */
    qz_block_template_t tmpl;
    bool have_work = false;
    uint32_t last_work_fetch = 0;

    while (s_mining) {
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
            } else {
                /* Fallback to local mining if node unreachable */
                if (!have_work) {
                    ESP_LOGW(TAG, "Node unreachable, mining locally");
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

                    /* Submit to node */
                    if (quartz_wifi_is_connected()) {
                        int rc = quartz_mining_submit(tmpl.job_id, nonce, header);
                        if (rc == 0) {
                            ESP_LOGI(TAG, "Block submitted and accepted!");
                        } else {
                            ESP_LOGW(TAG, "Block submit failed (%d)", rc);
                        }
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
            ESP_LOGI(TAG, "Mining... %lu H/s, %lu total, nonce %llu",
                     hps, s_hash_count, nonce);

#ifdef QUARTZ_HAS_DISPLAY
            if (quartz_display_get_screen() == QZ_SCREEN_MINING) {
                quartz_display_mining_stats(
                    s_hash_count,
                    hps,
                    s_blocks_found,
                    uptime,
                    quartz_wallet_get_address()
                );
            }
#endif

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
            /* Skip mining display update — QR stays on screen */
            s_hash_count++;
            if (s_hash_count % 100 == 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
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
    ESP_LOGI(TAG, "Quartz ESP32 Miner starting...");

    /* Initialize NVS */
    init_nvs();

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
