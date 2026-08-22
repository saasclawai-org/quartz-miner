/**
 * quartz_display_s3.c — ESP32-S3 OLED Display Driver (SSD1306 128x64)
 *
 * Targets the Heltec WiFi LoRa 32 V3 (ESP32-S3 + SSD1306 OLED):
 *   OLED I2C: SDA=41, SCL=42, addr 0x3C, RST=GPIO21, VEXT=GPIO36 (active low)
 * Falls back to LilyGO T3-class pins (SDA=17, SCL=18) via I2C probe.
 *
 * Battery: Heltec V3 VBAT → 1/2 divider → GPIO1 (ADC1_CH0).
 *   vbat = raw * 2 * 3.3 / 4095
 *
 * Implements the same quartz_display.h API as the M5Stack ILI9341
 * driver — monochrome rendering, colors map to on/off.
 *
 * Status screens:
 *   ID       — device ID (efuse-derived), enrollment, wallet, battery
 *   MINING   — animated mining indicator, hashrate, blocks, battery
 *   FLEET    — pool members, shares, rewards
 *   PAYMENT  — receive QR
 */

#include "quartz_display.h"
#include "quartz_qr.h"
#include "quartz.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef ESP_PLATFORM

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "QZ.DISP";

/* ---- Board pins (Heltec V3 defaults, T3 fallback) ---- */
#define OLED_SDA_HELTEC   41
#define OLED_SCL_HELTEC   42
#define OLED_SDA_T3       17
#define OLED_SCL_T3       18
#define OLED_RST          21
#define OLED_VEXT         36    /* active LOW powers the OLED on Heltec V3 */
#define OLED_I2C_ADDR     0x3C
#define BATT_ADC_GPIO     1     /* ADC1_CH0 on ESP32-S3 */
#define BATT_ADC_CHANNEL  ADC1_CHANNEL_0
#define BATT_DIVIDER      2.0f  /* 100k/100k divider */

#define DISP_W  128
#define DISP_H  64
#define FB_SIZE (DISP_W * DISP_H / 8)

static i2c_port_t s_i2c_port = I2C_NUM_0;
static bool s_ready = false;
static bool s_is_heltec = true;
static qz_screen_t s_screen = QZ_SCREEN_ID;

/* 1bpp framebuffer — page-major (SSD1306 native): fb[page][col] */
static uint8_t s_fb[FB_SIZE];

/* Battery state (refreshed on each stats redraw) */
static float s_vbat = 0.0f;
static uint8_t s_batt_pct = 0;
static bool s_batt_usb = false;     /* >4.25V → on USB/charging */
static bool s_batt_present = false; /* plausible LiPo reading */

/* Mining indicator animation state */
static uint8_t s_anim = 0;

/* ============================================================
 * Font — shared 8x16 table (also used by the ILI9341 driver).
 * Small text: rows 3..10 of each glyph (8x8 effective).
 * Big text:   full 8x16 glyph, scale x2 (16x32 huge).
 * ============================================================ */
#include "fonts/font8x16.h"

/* ============================================================
 * SSD1306 low level
 * ============================================================ */

static esp_err_t i2c_write(const uint8_t *data, size_t len) {
    return i2c_master_write_to_device(s_i2c_port, OLED_I2C_ADDR,
                                      data, len, pdMS_TO_TICKS(100));
}

static esp_err_t ssd_cmd(uint8_t cmd) {
    uint8_t buf[2] = { 0x00, cmd };
    return i2c_write(buf, 2);
}

static esp_err_t i2c_probe_pins(int sda, int scl) {
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = sda,
        .scl_io_num = scl,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000,
    };
    esp_err_t err = i2c_param_config(s_i2c_port, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(s_i2c_port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    /* Probe for the OLED */
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (OLED_I2C_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_stop(cmd);
    err = i2c_master_cmd_begin(s_i2c_port, cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void ssd1306_init_seq(void) {
    /* Standard SSD1306 128x64 init */
    ssd_cmd(0xAE);              /* display off */
    ssd_cmd(0x20); ssd_cmd(0x02); /* PAGE addressing (0xB0/0x00/0x10 used in flush) */
    ssd_cmd(0xB0);              /* page 0 */
    ssd_cmd(0xC8);              /* scan direction: COM remapped (flip vertical) */
    ssd_cmd(0x00);              /* low col */
    ssd_cmd(0x10);              /* high col */
    ssd_cmd(0x40);              /* start line 0 */
    ssd_cmd(0x81); ssd_cmd(0xCF); /* contrast */
    ssd_cmd(0xA1);              /* segment remap (flip horizontal — natural orientation) */
    ssd_cmd(0xA6);              /* normal display */
    ssd_cmd(0xA8); ssd_cmd(0x3F); /* multiplex 1/64 */
    ssd_cmd(0xA4);              /* output follows RAM */
    ssd_cmd(0xD3); ssd_cmd(0x00); /* display offset 0 */
    ssd_cmd(0xD5); ssd_cmd(0x80); /* clock div */
    ssd_cmd(0xD9); ssd_cmd(0xF1); /* pre-charge */
    ssd_cmd(0xDA); ssd_cmd(0x12); /* COM pins config */
    ssd_cmd(0xDB); ssd_cmd(0x40); /* VCOM detect */
    ssd_cmd(0x8D); ssd_cmd(0x14); /* charge pump on */
    ssd_cmd(0xAF);              /* display ON */
}

/* Push the full framebuffer to the display (page-address + 128 bytes
 * per transaction, 8 pages — plenty fast at 400 kHz) */
static void ssd_flush(void) {
    if (!s_ready) return;
    for (int page = 0; page < 8; page++) {
        ssd_cmd(0xB0 | page);          /* page address */
        ssd_cmd(0x00);                 /* column low nibble */
        ssd_cmd(0x10);                 /* column high nibble */
        uint8_t buf[1 + DISP_W];
        buf[0] = 0x40;                 /* data control byte */
        memcpy(buf + 1, s_fb + page * DISP_W, DISP_W);
        i2c_write(buf, sizeof(buf));
    }
}

/* ============================================================
 * Battery
 * ============================================================ */

static void battery_refresh(void) {
    /* 16-sample average for a steadier reading */
    uint32_t acc = 0;
    const int N = 16;
    for (int i = 0; i < N; i++) {
        int raw = adc1_get_raw(BATT_ADC_CHANNEL);
        acc += (raw > 0) ? (uint32_t)raw : 0;
    }
    float v = ((float)(acc / N) * 3.3f / 4095.0f) * BATT_DIVIDER;
    s_vbat = v;
    if (v > 4.25f) {
        s_batt_usb = true;
        s_batt_present = true;
        s_batt_pct = 100;
    } else if (v < 2.5f) {
        /* Floating pin or no battery installed — USB powered */
        s_batt_usb = true;
        s_batt_present = false;
        s_batt_pct = 0;
    } else {
        s_batt_usb = false;
        s_batt_present = true;
        int pct = (int)((v - 3.30f) / 0.90f * 100.0f);
        s_batt_pct = (uint8_t)(pct < 0 ? 0 : (pct > 100 ? 100 : pct));
    }
}

/* Draw battery glyph at (x,y). 16x8: outline + fill proportional. */
static void draw_battery(int x, int y) {
    /* outline */
    for (int i = 0; i < 14; i++) {
        s_fb[(y / 8) * DISP_W + x + i] = 0xFF;            /* top */
        s_fb[((y + 7) / 8) * DISP_W + x + i] = 0xFF;      /* bottom */
    }
    for (int j = 1; j < 7; j++) {
        int page = (y + j) / 8, bit = (y + j) % 8;
        s_fb[page * DISP_W + x] |= (1 << bit);
        s_fb[page * DISP_W + x + 13] |= (1 << bit);
        s_fb[page * DISP_W + x + 14] |= (1 << bit);       /* nub */
        s_fb[page * DISP_W + x + 15] |= (1 << bit);
    }
    /* fill = pct over 11 cells */
    int cells = (s_batt_pct * 11) / 100;
    if (s_batt_usb) cells = 11;
    for (int c = 0; c < cells; c++) {
        for (int j = 1; j < 7; j++) {
            int page = (y + j) / 8, bit = (y + j) % 8;
            s_fb[page * DISP_W + x + 2 + c] |= (1 << bit);
        }
    }
}

/* ============================================================
 * Framebuffer primitives
 * ============================================================ */

static inline void px_set(int x, int y, bool on) {
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    int page = y / 8;
    uint8_t *b = &s_fb[page * DISP_W + x];
    if (on) *b |= (1 << (y % 8));
    else    *b &= ~(1 << (y % 8));
}

void quartz_display_fill_rect(int x, int y, int w, int h, uint16_t color) {
    bool on = (color != QZ_COLOR_BLACK);
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            px_set(i, j, on);
}

void quartz_display_draw_pixel(int x, int y, uint16_t color) {
    px_set(x, y, color != QZ_COLOR_BLACK);
}

void quartz_display_draw_hline(int x, int y, int w, uint16_t color) {
    quartz_display_fill_rect(x, y, w, 1, color);
}

void quartz_display_clear(uint16_t color) {
    memset(s_fb, (color != QZ_COLOR_BLACK) ? 0xFF : 0x00, FB_SIZE);
}

/* Map any char to the font's 64-glyph table (ASCII 32-95, uppercase only).
 * Mirrors the ILI9341 driver: lowercase folds up, anything else → space. */
static uint8_t font_index(uint8_t ch) {
    if (ch >= 'a' && ch <= 'z') ch -= 32;
    if (ch < 32 || ch > 95) return 0;  /* space */
    return ch - 32;
}

/* Small text: 8x8 — rows 2..9 of each 8x16 glyph (verified body band) */
static void text8(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    bool fgon = (fg != QZ_COLOR_BLACK);
    bool bgon = (bg != QZ_COLOR_BLACK);
    while (*text && x <= DISP_W - 8) {
        const uint8_t *glyph = &quartz_font8x16[font_index((uint8_t)*text++)][0];
        for (int row = 0; row < 8; row++) {
            uint8_t bits = glyph[2 + row];   /* body band rows 2..9 */
            for (int col = 0; col < 8; col++)
                px_set(x + col, y + row, (bits & (0x80 >> col)) ? fgon : bgon);
        }
        x += 8;
    }
}

/* Scaled glyph blit — row-major font, MSB = leftmost pixel.
 * scale 1 → 8x16, scale 2 → 16x32 */
static void text_scaled(int x, int y, const char *text, int scale,
                        uint16_t fg, uint16_t bg) {
    bool fgon = (fg != QZ_COLOR_BLACK);
    bool bgon = (bg != QZ_COLOR_BLACK);
    while (*text) {
        const uint8_t *glyph = &quartz_font8x16[font_index((uint8_t)*text++)][0];
        for (int row = 0; row < 16; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < 8; col++) {
                bool on = (bits & (0x80 >> col)) ? fgon : bgon;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++)
                        px_set(x + col * scale + sx, y + row * scale + sy, on);
            }
        }
        x += 8 * scale;
        if (x >= DISP_W) break;
    }
}

void quartz_display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    text8(x, y, text, fg, bg);
}
void quartz_display_draw_text_s(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    text8(x, y, text, fg, bg);
}
void quartz_display_draw_text_l(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    text8(x, y, text, fg, bg);
}
void quartz_display_draw_big_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    text_scaled(x, y, text, 1, fg, bg);   /* 8x16 */
}
void quartz_display_draw_huge_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    text_scaled(x, y, text, 2, fg, bg);   /* 16x32 */
}

/* ============================================================
 * Init
 * ============================================================ */

void quartz_display_init(void) {
    /* Heltec V3: VEXT must be LOW to power the OLED */
    gpio_config_t vc = {
        .pin_bit_mask = 1ULL << OLED_VEXT,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&vc);
    gpio_set_level(OLED_VEXT, 0);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* OLED reset (Heltec V3) */
    gpio_config_t rc = vc;
    rc.pin_bit_mask = 1ULL << OLED_RST;
    gpio_config(&rc);
    gpio_set_level(OLED_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(OLED_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Probe I2C: Heltec V3 pins first, then T3-class */
    if (i2c_probe_pins(OLED_SDA_HELTEC, OLED_SCL_HELTEC) == ESP_OK) {
        s_is_heltec = true;
    } else {
        i2c_driver_delete(s_i2c_port);
        if (i2c_probe_pins(OLED_SDA_T3, OLED_SCL_T3) == ESP_OK) {
            s_is_heltec = false;
        } else {
            ESP_LOGW(TAG, "No SSD1306 found — staying headless");
            return;
        }
    }

    /* Battery ADC: GPIO1, 11dB attenuation for ~3.3V full scale */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(BATT_ADC_CHANNEL, ADC_ATTEN_DB_11);
    battery_refresh();

    ssd1306_init_seq();
    quartz_display_clear(QZ_COLOR_BLACK);
    s_ready = true;
    ESP_LOGI(TAG, "SSD1306 OLED ready (%s pins, vbat=%.2fV)",
             s_is_heltec ? "Heltec V3" : "T3", s_vbat);
}

void quartz_display_set_screen(qz_screen_t screen) { s_screen = screen; }
qz_screen_t quartz_display_get_screen(void) { return s_screen; }

/* ============================================================
 * Screens
 * ============================================================ */

void quartz_display_splash(void) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_draw_huge_text(4, 0, "QUARTZ", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    quartz_display_draw_text(16, 34, "QUARTZ TESTNET MINER", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    quartz_display_draw_text(28, 46, FW_VERSION_STRING, QZ_COLOR_PURPLE, QZ_COLOR_BLACK);
    quartz_display_draw_text(25, 56, "quartz.saasclaw.ai", QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    ssd_flush();
}

/* Shared status header: brand + battery.
 * Returns y of first free row. */
static int status_header(const char *title) {
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    text8(1, 0, title, QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    battery_refresh();
    draw_battery(DISP_W - 26, 0);
    if (s_batt_present && !s_batt_usb) {
        char p[5];
        snprintf(p, sizeof(p), "%d%%", s_batt_pct);
        text8(DISP_W - 26 + 17, 0, p, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    } else {
        text8(DISP_W - 26 + 17, 0, "USB", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    }
    return 12;
}

/* Animated mining indicator: pixel-art spinner + activity bars.
 * 22px wide, 8px tall at (x, y). '|' is not in the 64-glyph font —
 * so the spinner frames are bitmaps, not font chars. */
static const uint8_t spin_frames[4][8] = {
    {0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18},  /* | */
    {0x03,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80},  /* / */
    {0x00,0x00,0x00,0xFF,0xFF,0x00,0x00,0x00},  /* - */
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x03,0x01},  /* \ */
};

static void draw_mining_indicator(int x, int y, bool active) {
    if (active) {
        const uint8_t *f = spin_frames[s_anim & 3];
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                px_set(x + col, y + row, f[row] & (0x80 >> col));
    } else {
        text8(x, y, "IDLE", QZ_COLOR_DARKGRAY, QZ_COLOR_BLACK);
        return;
    }
    /* Equalizer bars — heights cycle with animation counter */
    static const uint8_t heights[3][4] = {
        {3, 6, 4, 2}, {5, 3, 7, 4}, {2, 7, 3, 6},
    };
    const uint8_t *h = heights[s_anim % 3];
    for (int b = 0; b < 4; b++) {
        for (int j = 0; j < h[b]; j++)
            px_set(x + 12 + b * 3, y + 8 - j, true);
    }
}

/* Compact wallet form: lead..tail, fits a 128px line */
static void short_addr_fmt(const char *addr, int lead, int tail, char *out, size_t outsz) {
    size_t len = addr ? strlen(addr) : 0;
    if (len > (size_t)(lead + tail + 2)) {
        snprintf(out, outsz, "%.*s..%s", lead, addr, addr + len - tail);
    } else {
        snprintf(out, outsz, "%s", addr ? addr : "-");
    }
}

void quartz_display_mining_stats(
    uint32_t hash_count, uint32_t hash_rate,
    uint32_t blocks_found, uint32_t uptime_sec,
    const char *wallet_address)
{
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    int y = status_header("QUARTZ MINER");

    /* Mining indicator + hashrate */
    draw_mining_indicator(2, y, hash_rate > 0);
    char buf[24];
    snprintf(buf, sizeof(buf), "%lu H/s", (unsigned long)hash_rate);
    text8(40, y, buf, QZ_COLOR_CYAN, QZ_COLOR_BLACK);

    /* Device identity — wallet short form */
    y += 12;
    if (wallet_address && strlen(wallet_address) > 12) {
        char sa[16];
        short_addr_fmt(wallet_address, 6, 4, sa, sizeof(sa));
        snprintf(buf, sizeof(buf), "ID %s", sa);
    } else {
        snprintf(buf, sizeof(buf), "ID %s", wallet_address ? wallet_address : "-");
    }
    text8(2, y, buf, QZ_COLOR_YELLOW, QZ_COLOR_BLACK);

    /* Blocks / hashes */
    y += 11;
    snprintf(buf, sizeof(buf), "BLK %lu   HASH %lu",
             (unsigned long)blocks_found, (unsigned long)hash_count);
    text8(2, y, buf, QZ_COLOR_WHITE, QZ_COLOR_BLACK);

    /* Uptime + battery volts */
    y += 11;
    uint32_t hrs = uptime_sec / 3600, mins = (uptime_sec % 3600) / 60;
    snprintf(buf, sizeof(buf), "UP %luh%02lum", (unsigned long)hrs, (unsigned long)mins);
    text8(2, y, buf, QZ_COLOR_GREEN, QZ_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%.2fV", s_vbat);
    text8(84, y, buf, s_batt_usb ? QZ_COLOR_CYAN : QZ_COLOR_ORANGE, QZ_COLOR_BLACK);

    s_anim++;
    ssd_flush();
}

void quartz_display_id_screen(
    bool enrolled,
    const uint8_t device_id[32],
    uint32_t uptime_sec,
    uint32_t hash_rate,
    const char *wallet_address)
{
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    int y = status_header(enrolled ? "QUARTZ [ENROLLED]" : "QUARTZ [DORMANT]");

    char buf[24];
    text8(2, y, "DEVICE ID", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "QZ-%02X%02X%02X%02X",
             device_id[0], device_id[1], device_id[2], device_id[3]);
    text8(2, y + 11, buf, QZ_COLOR_CYAN, QZ_COLOR_BLACK);

    y += 23;
    text8(2, y, "WALLET", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    if (wallet_address && strlen(wallet_address) > 14) {
        char sa[16];
        short_addr_fmt(wallet_address, 8, 4, sa, sizeof(sa));
        text8(2, y + 11, sa, QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
    } else {
        text8(2, y + 11, wallet_address ? wallet_address : "-", QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
    }

    y += 23;
    uint32_t days = uptime_sec / 86400, hrs = (uptime_sec % 86400) / 3600,
             mins = (uptime_sec % 3600) / 60;
    if (days) snprintf(buf, sizeof(buf), "UP %lud %luh", (unsigned long)days, (unsigned long)hrs);
    else if (hrs) snprintf(buf, sizeof(buf), "UP %luh %lum", (unsigned long)hrs, (unsigned long)mins);
    else snprintf(buf, sizeof(buf), "UP %lum", (unsigned long)mins);
    text8(2, y, buf, QZ_COLOR_GREEN, QZ_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%lu H/s", (unsigned long)hash_rate);
    text8(78, y, buf, QZ_COLOR_GRAY, QZ_COLOR_BLACK);

    ssd_flush();
}

void quartz_display_fleet_screen(
    uint32_t member_count,
    uint32_t my_shares,
    uint64_t rewards_qz_milli,
    uint32_t blocks_found)
{
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    int y = status_header("FLEET");

    char buf[26];
    snprintf(buf, sizeof(buf), "MEMBERS  %lu / 100", (unsigned long)member_count);
    text8(2, y, buf, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    /* progress bar toward the 100-device goal */
    int fill = (int)((member_count > 100 ? 100 : member_count) * 124 / 100);
    quartz_display_draw_hline(2, y + 9, 124, QZ_COLOR_DARKGRAY);
    quartz_display_fill_rect(2, y + 9, fill, 1, QZ_COLOR_GREEN);

    y += 14;
    snprintf(buf, sizeof(buf), "SHARES  %lu", (unsigned long)my_shares);
    text8(2, y, buf, QZ_COLOR_WHITE, QZ_COLOR_BLACK);

    y += 11;
    snprintf(buf, sizeof(buf), "REWARDS %.2f QZ", rewards_qz_milli / 1000.0);
    text8(2, y, buf, QZ_COLOR_YELLOW, QZ_COLOR_BLACK);

    y += 11;
    snprintf(buf, sizeof(buf), "BLOCKS  %lu", (unsigned long)blocks_found);
    text8(2, y, buf, QZ_COLOR_CYAN, QZ_COLOR_BLACK);

    ssd_flush();
}

void quartz_display_connecting(void) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_draw_big_text(28, 8, "CONNECTING", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    quartz_display_draw_text(22, 30, "Looking for testnet node...", QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    draw_mining_indicator(52, 44, true);
    ssd_flush();
}

void quartz_display_error(const char *msg) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    text8(1, 0, "ERROR", QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    /* wrap message at 15 chars/line, up to 5 lines */
    if (msg) {
        int y = 14, len = strlen(msg);
        for (int off = 0; off < len && y < 60; off += 15) {
            char line[16];
            int n = len - off < 15 ? len - off : 15;
            memcpy(line, msg + off, n);
            line[n] = '\0';
            text8(2, y, line, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
            y += 10;
        }
    }
    ssd_flush();
}

void quartz_display_seed_phrase(const char words[12][12], int page) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    char hdr[22];
    snprintf(hdr, sizeof(hdr), "SEED WORDS %d-%d", page * 6 + 1, page * 6 + 6);
    text8(1, 0, hdr, QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    for (int i = 0; i < 6; i++) {
        char line[17];
        snprintf(line, sizeof(line), "%2d. %s", page * 6 + i + 1, words[page * 6 + i]);
        text8(2, 12 + i * 9, line, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    }
    ssd_flush();
}

void quartz_display_message(const char *from, const char *text, int block_height) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    int y = status_header("MESSAGE");
    char buf[20];
    snprintf(buf, sizeof(buf), "From %s", from ? from : "?");
    text8(2, y, buf, QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
    y += 11;
    snprintf(buf, sizeof(buf), "Block %d", block_height);
    text8(2, y, buf, QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    y += 11;
    if (text) {
        int len = strlen(text);
        for (int off = 0; off < len && y < 64; off += 15) {
            char line[16];
            int n = len - off < 15 ? len - off : 15;
            memcpy(line, text + off, n);
            line[n] = '\0';
            text8(2, y, line, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
            y += 9;
        }
    }
    ssd_flush();
}

void quartz_display_portal(const char *ap_name) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    text8(1, 0, "WIFI SETUP", QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    text8(2, 14, "1. Join WiFi:", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    text8(10, 24, ap_name ? ap_name : "Quartz-AP", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    text8(2, 36, "2. Open browser:", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    text8(10, 46, "192.168.4.1", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    ssd_flush();
}

void quartz_display_qr_payment(const char *address, float amount) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);

    char qr_str[96];
    snprintf(qr_str, sizeof(qr_str), "quartz:%s?amount=%.2f",
             address ? address : "", amount);

    int qr_len = strlen(qr_str);
    int version = quartz_qr_version_for_data(qr_len, QR_ECC_LOW);
    int modules = 21 + 4 * (version - 1);

    /* Screen is black around the code — the display itself acts as the
     * quiet zone, so maximize scale within 128x64. */
    int scale = 64 / modules;
    if (scale < 1) scale = 1;
    if (scale > 2) scale = 2;  /* keep modules crisp at 2px/module */

    int qr_px = modules * scale;
    int qr_x = (DISP_W - qr_px) / 2;
    int qr_y = (DISP_H - qr_px) / 2;
    if (qr_y < 0) qr_y = 0;

    int rc = quartz_qr_display(qr_str, QR_ECC_LOW, qr_x, qr_y, scale,
                               QZ_COLOR_BLACK, QZ_COLOR_WHITE);

    if (rc != 0) {
        /* fallback: show address as text */
        quartz_display_clear(QZ_COLOR_BLACK);
        text8(2, 2, "PAY TO", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
        if (address) {
            int len = strlen(address);
            int y = 14;
            for (int off = 0; off < len && y < 64; off += 15) {
                char line[16];
                int n = len - off < 15 ? len - off : 15;
                memcpy(line, address + off, n);
                line[n] = '\0';
                text8(2, y, line, QZ_COLOR_CYAN, QZ_COLOR_BLACK);
                y += 9;
            }
        }
    } else if (scale == 1) {
        /* small footer with amount when there's room */
        char amt[20];
        snprintf(amt, sizeof(amt), "%.2f QZ", amount);
        text8((DISP_W - (int)strlen(amt) * 8) / 2, 56, amt, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    }
    ssd_flush();
}

/* === PIN entry === */
void quartz_display_pin_entry(const char *entered_pin, int attempts_left) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    text8(1, 0, "ENTER PIN", QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    char dots[12] = {0};
    int n = entered_pin ? strlen(entered_pin) : 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) dots[i] = '*';
    quartz_display_draw_big_text((DISP_W - n * 8) / 2, 20, dots,
                                 QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "%d tries left", attempts_left);
    text8((DISP_W - (int)strlen(buf) * 8) / 2, 48, buf, QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    ssd_flush();
}

void quartz_display_pin_entry_m5stack(int digit, int pin_len, int attempts_left) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    quartz_display_fill_rect(0, 0, DISP_W, 9, QZ_COLOR_WHITE);
    text8(1, 0, "SET PIN", QZ_COLOR_BLACK, QZ_COLOR_WHITE);
    char d[2] = { (char)('0' + (digit % 10)), 0 };
    quartz_display_draw_huge_text(56, 12, d, QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    char buf[20];
    snprintf(buf, sizeof(buf), "LEN %d", pin_len);
    text8(2, 48, buf, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    snprintf(buf, sizeof(buf), "%d left", attempts_left);
    text8(76, 48, buf, QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    ssd_flush();
}

/* === Recovery === */
void quartz_display_recovery_start(void) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    text8(20, 4, "RECOVERY MODE", QZ_COLOR_YELLOW, QZ_COLOR_BLACK);
    text8(4, 24, "Waiting for seed on", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    text8(4, 34, "serial console...", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    ssd_flush();
}

void quartz_display_recovery_sync(const char *address) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    text8(16, 4, "RECOVERING...", QZ_COLOR_CYAN, QZ_COLOR_BLACK);
    text8(4, 24, "Deriving keys + sync", QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    if (address) text8(4, 40, address, QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    ssd_flush();
}

void quartz_display_recovery_done(const char *address, int sig_index, int balance) {
    if (!s_ready) return;
    quartz_display_clear(QZ_COLOR_BLACK);
    text8(28, 4, "RECOVERED!", QZ_COLOR_GREEN, QZ_COLOR_BLACK);
    char buf[24];
    snprintf(buf, sizeof(buf), "Key %d  %d QZ", sig_index, balance);
    text8(4, 22, buf, QZ_COLOR_WHITE, QZ_COLOR_BLACK);
    if (address) text8(4, 38, address, QZ_COLOR_GRAY, QZ_COLOR_BLACK);
    ssd_flush();
}

void quartz_display_recovery_error(const char *msg) {
    quartz_display_error(msg);
}

#endif /* ESP_PLATFORM */
