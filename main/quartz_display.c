/**
 * quartz_display.c — M5Stack Core ILI9341 Display Driver
 *
 * Clean rewrite. Three screens: splash → mining → QR payment.
 * Pins: MOSI=23, CLK=18, CS=14, DC=27, RST=33, BL=32
 */

#include "quartz_display.h"
#include "quartz_qr.h"
#include "fonts/font_s.h"
#include "fonts/font_l.h"
#include <string.h>
#include <stdio.h>

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#endif

static const char *TAG = "QZ.DISP";

/* M5Stack Core display pins */
#define PIN_MOSI   23
#define PIN_CLK    18
#define PIN_CS     14
#define PIN_DC     27
#define PIN_RST    33
#define PIN_BL     32

#define DISP_W  320
#define DISP_H  240
#define FONT_W  8
#define FONT_H  16

static spi_device_handle_t s_spi = NULL;
static bool s_ready = false;
static qz_screen_t s_screen = QZ_SCREEN_MINING;

/* 8x16 font shared with the S3 OLED driver */
#include "fonts/font8x16.h"
#define font8x16 quartz_font8x16

/* ============================================================
 * Low-level SPI helpers
 * ============================================================ */

#ifdef ESP_PLATFORM
static void spi_cmd(uint8_t cmd) {
    gpio_set_level(PIN_DC, 0);
    spi_transaction_t t = {0};
    t.length = 8;
    t.tx_buffer = &cmd;
    spi_device_polling_transmit(s_spi, &t);
}

static void spi_data(const uint8_t *data, int len) {
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = {0};
    t.length = len * 8;
    t.tx_buffer = data;
    spi_device_polling_transmit(s_spi, &t);
}

static void spi_data1(uint8_t d) {
    spi_data(&d, 1);
}

static void set_addr_window(int x1, int y1, int x2, int y2) {
    spi_cmd(0x2A); /* Column */
    uint8_t cx[4] = {(x1>>8)&0xFF, x1&0xFF, (x2>>8)&0xFF, x2&0xFF};
    spi_data(cx, 4);
    spi_cmd(0x2B); /* Row */
    uint8_t ry[4] = {(y1>>8)&0xFF, y1&0xFF, (y2>>8)&0xFF, y2&0xFF};
    spi_data(ry, 4);
    spi_cmd(0x2C); /* Write */
}
#endif

/* ============================================================
 * Public drawing primitives
 * ============================================================ */

void quartz_display_init(void) {
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "Initializing ILI9341 display");

    /* GPIO: backlight, DC, RST, CS */
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL<<PIN_BL) | (1ULL<<PIN_DC) | (1ULL<<PIN_RST) | (1ULL<<PIN_CS);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_BL, 0);   /* Backlight off during init */
    gpio_set_level(PIN_RST, 1);

    /* SPI bus — FULL DUPLEX (ILI9341 is 4-wire SPI, not 3-wire)
     * Half-duplex mode corrupts DMA transfers on ESP32. */
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.miso_io_num = -1;   /* No MISO — write-only display */
    buscfg.sclk_io_num = PIN_CLK;
    buscfg.max_transfer_sz = DISP_W * DISP_H * 2 + 8;
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 27 * 1000 * 1000;  /* 27MHz — M5Stack standard */
    devcfg.mode = 0;
    devcfg.spics_io_num = PIN_CS;
    devcfg.queue_size = 7;
    /* NO HALFDUPLEX flag — ILI9341 uses standard 4-wire SPI */
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);

    /* Hardware reset */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* ---- ILI9341 init sequence (from M5Stack Arduino library) ---- */

    /* Power control B */
    spi_cmd(0xCF); spi_data1(0x00); spi_data1(0xC1); spi_data1(0x30);
    /* Power on sequence control */
    spi_cmd(0xED); spi_data1(0x64); spi_data1(0x03); spi_data1(0x12); spi_data1(0x81);
    /* Driver timing control A */
    spi_cmd(0xE8); spi_data1(0x85); spi_data1(0x00); spi_data1(0x78);
    /* Power control A */
    spi_cmd(0xCB); spi_data1(0x39); spi_data1(0x2C); spi_data1(0x00);
    spi_data1(0x34); spi_data1(0x02);
    /* Pump ratio control */
    spi_cmd(0xF7); spi_data1(0x20);
    /* Driver timing control B */
    spi_cmd(0xEA); spi_data1(0x00); spi_data1(0x00);

    /* Power control 1 */
    spi_cmd(0xC0); spi_data1(0x23);
    /* Power control 2 */
    spi_cmd(0xC1); spi_data1(0x10);
    /* VCOM control 1 */
    spi_cmd(0xC5); spi_data1(0x3E); spi_data1(0x28);
    /* VCOM control 2 */
    spi_cmd(0xC7); spi_data1(0x86);

    /* MADCTL — M5Stack landscape orientation with BGR color order */
    spi_cmd(0x36); spi_data1(0x60);  /* MX + MV, landscape, RGB pixel order */
    ESP_LOGI(TAG, "MADCTL = 0x60 (landscape RGB)");

    /* Pixel format: 16-bit */
    spi_cmd(0x3A); spi_data1(0x55);

    /* Frame rate control */
    spi_cmd(0xB1); spi_data1(0x00); spi_data1(0x18);
    /* Display function control */
    spi_cmd(0xB6); spi_data1(0x08); spi_data1(0x82); spi_data1(0x27);

    /* 3Gamma function disable */
    spi_cmd(0xF2); spi_data1(0x00);
    /* Gamma curve selected */
    spi_cmd(0x26); spi_data1(0x01);

    /* Positive gamma correction */
    spi_cmd(0xE0);
    spi_data1(0x0F); spi_data1(0x31); spi_data1(0x2B); spi_data1(0x0C);
    spi_data1(0x0E); spi_data1(0x08); spi_data1(0x4E); spi_data1(0xF1);
    spi_data1(0x37); spi_data1(0x07); spi_data1(0x10); spi_data1(0x03);
    spi_data1(0x0E); spi_data1(0x09); spi_data1(0x00);

    /* Negative gamma correction */
    spi_cmd(0xE1);
    spi_data1(0x00); spi_data1(0x0E); spi_data1(0x14); spi_data1(0x03);
    spi_data1(0x11); spi_data1(0x07); spi_data1(0x31); spi_data1(0xC1);
    spi_data1(0x48); spi_data1(0x08); spi_data1(0x0F); spi_data1(0x0C);
    spi_data1(0x31); spi_data1(0x36); spi_data1(0x0F);

    /* Sleep out */
    spi_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* Display ON */
    spi_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* Backlight on */
    gpio_set_level(PIN_BL, 1);

    s_ready = true;
    ESP_LOGI(TAG, "Display ready");
#endif
}

void quartz_display_clear(uint16_t color) {
    quartz_display_fill_rect(0, 0, DISP_W, DISP_H, color);
}

void quartz_display_fill_rect(int x, int y, int w, int h, uint16_t color) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > DISP_W) w = DISP_W - x;
    if (y + h > DISP_H) h = DISP_H - y;
    if (w <= 0 || h <= 0) return;

    set_addr_window(x, y, x+w-1, y+h-1);

    gpio_set_level(PIN_DC, 1);
    int total = w * h;
    int chunk = 1024;
    uint16_t *buf = heap_caps_malloc(chunk * 2, MALLOC_CAP_8BIT);
    if (!buf) return;
    for (int i = 0; i < chunk; i++) buf[i] = color;

    int sent = 0;
    while (sent < total) {
        int n = total - sent;
        if (n > chunk) n = chunk;
        spi_transaction_t t = {0};
        t.length = n * 16;
        t.tx_buffer = buf;
        spi_device_polling_transmit(s_spi, &t);
        sent += n;
    }
    free(buf);
#endif
}

void quartz_display_draw_pixel(int x, int y, uint16_t color) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    set_addr_window(x, y, x, y);
    gpio_set_level(PIN_DC, 1);
    spi_transaction_t t = {0};
    t.length = 16;
    t.tx_buffer = &color;
    spi_device_polling_transmit(s_spi, &t);
#endif
}

void quartz_display_draw_hline(int x, int y, int w, uint16_t color) {
    quartz_display_fill_rect(x, y, w, 1, color);
}

void quartz_display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    int i = 0;
    while (text[i]) {
        unsigned char c = (unsigned char)text[i];
        /* Convert lowercase to uppercase (font only has uppercase) */
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 95) c = 0;  /* map unknown to space */
        else c -= 32;
        const uint8_t *glyph = font8x16[c];

        set_addr_window(x + i * FONT_W, y, x + i * FONT_W + FONT_W - 1, y + FONT_H - 1);
        gpio_set_level(PIN_DC, 1);

        uint16_t charbuf[FONT_W * FONT_H];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                charbuf[row * FONT_W + col] = (bits & (0x80 >> col)) ? fg : bg;
            }
        }
        spi_transaction_t t = {0};
        t.length = FONT_W * FONT_H * 16;
        t.tx_buffer = charbuf;
        spi_device_polling_transmit(s_spi, &t);
        i++;
    }
#endif
}

void quartz_display_draw_big_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
    /* 2x scale: 16x32 per char */
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    int i = 0;
    while (text[i]) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 95) c = 0;
        else c -= 32;
        const uint8_t *glyph = font8x16[c];

        int bx = x + i * FONT_W * 2;
        set_addr_window(bx, y, bx + FONT_W * 2 - 1, y + FONT_H * 2 - 1);
        gpio_set_level(PIN_DC, 1);

        uint16_t charbuf[FONT_W * 2 * FONT_H * 2];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
                /* 2x2 block */
                charbuf[(row*2)   * (FONT_W*2) + col*2]     = color;
                charbuf[(row*2)   * (FONT_W*2) + col*2 + 1] = color;
                charbuf[(row*2+1) * (FONT_W*2) + col*2]     = color;
                charbuf[(row*2+1) * (FONT_W*2) + col*2 + 1] = color;
            }
        }
        spi_transaction_t t = {0};
        t.length = FONT_W * 2 * FONT_H * 2 * 16;
        t.tx_buffer = charbuf;
        spi_device_polling_transmit(s_spi, &t);
        i++;
    }
#endif
}

/* 4x scale: 32x64 per char */
void quartz_display_draw_huge_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);
void quartz_display_draw_huge_text(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    int i = 0;
    while (text[i]) {
        unsigned char c = (unsigned char)text[i];
        if (c >= 'a' && c <= 'z') c -= 32;
        if (c < 32 || c > 95) c = 0;
        else c -= 32;
        const uint8_t *glyph = font8x16[c];

        int bx = x + i * FONT_W * 4;
        set_addr_window(bx, y, bx + FONT_W * 4 - 1, y + FONT_H * 4 - 1);
        gpio_set_level(PIN_DC, 1);

        /* 32x64 = 2048 pixels = 4096 bytes — under SPI max */
        uint16_t charbuf[FONT_W * 4 * FONT_H * 4];
        for (int row = 0; row < FONT_H; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < FONT_W; col++) {
                uint16_t color = (bits & (0x80 >> col)) ? fg : bg;
                for (int dy = 0; dy < 4; dy++) {
                    for (int dx = 0; dx < 4; dx++) {
                        charbuf[(row*4+dy) * (FONT_W*4) + col*4+dx] = color;
                    }
                }
            }
        }
        spi_transaction_t t = {0};
        t.length = FONT_W * 4 * FONT_H * 4 * 16;
        t.tx_buffer = charbuf;
        spi_device_polling_transmit(s_spi, &t);
        i++;
    }
#endif
}

/* ============================================================
 * DejaVu Sans font rendering (12px and 16px)
 * ============================================================ */

/* Render text using small DejaVu font (8x14) */
void quartz_display_draw_text_s(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    int i = 0;
    while (text[i]) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126) c = 32;
        c -= 32;
        const uint8_t *glyph = font_s[c];
        int cw = FONT_S_W;
        int ch = FONT_S_H;
        
        int bx = x + i * cw;
        set_addr_window(bx, y, bx + cw - 1, y + ch - 1);
        gpio_set_level(PIN_DC, 1);
        
        uint16_t charbuf[cw * ch];
        for (int row = 0; row < ch; row++) {
            uint8_t bits = glyph[row];
            for (int col = 0; col < cw; col++) {
                charbuf[row * cw + col] = (bits & (0x80 >> col)) ? fg : bg;
            }
        }
        spi_transaction_t t = {0};
        t.length = cw * ch * 16;
        t.tx_buffer = charbuf;
        spi_device_polling_transmit(s_spi, &t);
        i++;
    }
#endif
}

/* Render text using large DejaVu font (16x18) */
void quartz_display_draw_text_l(int x, int y, const char *text, uint16_t fg, uint16_t bg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    int i = 0;
    while (text[i]) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c > 126) c = 32;
        c -= 32;
        const uint8_t *glyph = font_l[c];
        int cw = FONT_L_W;
        int ch = FONT_L_H;
        
        int bx = x + i * cw;
        set_addr_window(bx, y, bx + cw - 1, y + ch - 1);
        gpio_set_level(PIN_DC, 1);
        
        /* 16x18 = 288 pixels = 576 bytes — under SPI max */
        uint16_t charbuf[cw * ch];
        for (int row = 0; row < ch; row++) {
            uint8_t hi = glyph[row * 2];
            uint8_t lo = glyph[row * 2 + 1];
            for (int col = 0; col < cw; col++) {
                uint8_t bit;
                if (col < 8) {
                    bit = hi & (0x80 >> col);
                } else {
                    bit = lo & (0x80 >> (col - 8));
                }
                charbuf[row * cw + col] = bit ? fg : bg;
            }
        }
        spi_transaction_t t = {0};
        t.length = cw * ch * 16;
        t.tx_buffer = charbuf;
        spi_device_polling_transmit(s_spi, &t);
        i++;
    }
#endif
}

/* ============================================================
 * Screen management
 * ============================================================ */

void quartz_display_set_screen(qz_screen_t screen) { s_screen = screen; }
qz_screen_t quartz_display_get_screen(void) { return s_screen; }

/* ============================================================
 * Screens
 * ============================================================ */

/* Colors */
#define COL_BG     0x18E3   /* dark blue-purple */
#define COL_CARD   0x2104   /* slightly lighter */
#define COL_ACCENT 0x9933   /* Quartz purple */
#define COL_CYAN   0x07FF
#define COL_GREEN  0x07E0
#define COL_YELLOW 0xFFE0
#define COL_RED    0xF800
#define COL_WHITE  0xFFFF
#define COL_GRAY   0x8410
#define COL_DARKGRAY 0x4208

/* === Splash Screen === */
void quartz_display_splash(void) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;

    quartz_display_clear(COL_BG);

    /* Purple header bar */
    quartz_display_fill_rect(0, 0, DISP_W, 50, COL_ACCENT);

    /* QUARTZ title - large DejaVu */
    const char *title = "QUARTZ";
    int title_px = strlen(title) * 16;  /* 16px per char */
    int title_x = (DISP_W - title_px) / 2;
    quartz_display_draw_text_l(title_x, 16, title, COL_WHITE, COL_ACCENT);

    /* Subtitle */
    const char *sub = "ESP32 CRYPTOCURRENCY MINER";
    int sub_px = strlen(sub) * 8;
    quartz_display_draw_text((DISP_W - sub_px) / 2, 60, sub, COL_CYAN, COL_BG);

    /* Loading indicator */
    quartz_display_draw_text_l(90, 110, "STARTING...", COL_GRAY, COL_BG);

    vTaskDelay(pdMS_TO_TICKS(1500));
#endif
}

/* === Mining Screen === */
void quartz_display_mining_stats(
    uint32_t hash_count, uint32_t hash_rate,
    uint32_t blocks_found, uint32_t uptime_sec,
    const char *wallet_address)
{
#ifdef ESP_PLATFORM
    if (!s_ready) return;

    /* Background */
    quartz_display_clear(COL_BG);

    /* Top bar */
    quartz_display_fill_rect(0, 0, DISP_W, 24, COL_ACCENT);
    quartz_display_draw_text_s(8, 4, "QUARTZ MINER", COL_WHITE, COL_ACCENT);

    /* Connection status (top right) */
    quartz_display_draw_text_s(DISP_W - 40, 4, "SOLO", COL_YELLOW, COL_ACCENT);

    /* Separator */
    quartz_display_fill_rect(0, 24, DISP_W, 2, COL_ACCENT);

    /* Hashrate — large */
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", hash_rate);
    quartz_display_draw_text_l(8, 30, "HASHRATE", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(8, 50, buf, COL_CYAN, COL_BG);

    /* Blocks found */
    snprintf(buf, sizeof(buf), "%lu", blocks_found);
    quartz_display_draw_text_l(8, 104, "BLOCKS", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(8, 124, buf, COL_GREEN, COL_BG);

    /* Uptime */
    uint32_t hrs = uptime_sec / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    snprintf(buf, sizeof(buf), "%luh %lum", hrs, mins);
    quartz_display_draw_text_l(170, 104, "UPTIME", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(170, 124, buf, COL_WHITE, COL_BG);

    /* Wallet address (truncated) */
    quartz_display_draw_text_l(8, 168, "WALLET", COL_WHITE, COL_BG);
    if (wallet_address && strlen(wallet_address) > 34) {
        char short_addr[20];
        memcpy(short_addr, wallet_address, 8);
        short_addr[8] = '.'; short_addr[9] = '.'; short_addr[10] = '.';
        memcpy(short_addr + 11, wallet_address + strlen(wallet_address) - 6, 6);
        short_addr[17] = '\0';
        quartz_display_draw_text_l(8, 188, short_addr, COL_YELLOW, COL_BG);
    } else if (wallet_address) {
        quartz_display_draw_text_l(8, 188, wallet_address, COL_YELLOW, COL_BG);
    }

    /* Button hints at bottom */
    quartz_display_fill_rect(0, 220, DISP_W, 20, COL_CARD);
    quartz_display_draw_text_s(8, 223, "[A] Pay", COL_ACCENT, COL_CARD);
    quartz_display_draw_text_s(120, 223, "[B] Wallet", COL_GRAY, COL_CARD);
    quartz_display_draw_text_s(230, 223, "[C] Msgs", COL_GRAY, COL_CARD);
#endif
}

/* === Identity Screen (boot default) ===
 * Design: the hero is the identity — enrolled status + device ID.
 * Hashrate is deliberately a small gray line (it pays nothing).
 */
void quartz_display_id_screen(
    bool enrolled,
    const uint8_t device_id[32],
    uint32_t uptime_sec,
    uint32_t hash_rate,
    const char *wallet_address)
{
#ifdef ESP_PLATFORM
    if (!s_ready) return;

    quartz_display_clear(COL_BG);

    /* Top bar */
    quartz_display_fill_rect(0, 0, DISP_W, 24, COL_ACCENT);
    quartz_display_draw_text_s(8, 4, "QUARTZ", COL_WHITE, COL_ACCENT);
    if (enrolled) {
        quartz_display_draw_text_s(DISP_W - 96, 4, "ENROLLED", COL_GREEN, COL_ACCENT);
        quartz_display_draw_text_s(DISP_W - 22, 4, "OK", COL_GREEN, COL_ACCENT);
    } else {
        quartz_display_draw_text_s(DISP_W - 76, 4, "DORMANT", COL_YELLOW, COL_ACCENT);
    }
    quartz_display_fill_rect(0, 24, DISP_W, 2, COL_ACCENT);

    /* Device ID — hero (16×32 big font) */
    char buf[40];
    quartz_display_draw_text_l(8, 36, "DEVICE ID", COL_WHITE, COL_BG);
    snprintf(buf, sizeof(buf), "QZ-%02X%02X%02X%02X",
             device_id[0], device_id[1], device_id[2], device_id[3]);
    quartz_display_draw_big_text(8, 56, buf, COL_CYAN, COL_BG);

    /* Uptime — identity age */
    uint32_t days = uptime_sec / 86400;
    uint32_t hrs  = (uptime_sec % 86400) / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    if (days > 0) snprintf(buf, sizeof(buf), "%lud %luh", days, hrs);
    else if (hrs > 0) snprintf(buf, sizeof(buf), "%luh %lum", hrs, mins);
    else snprintf(buf, sizeof(buf), "%lum", mins);
    quartz_display_draw_text_l(8, 104, "UPTIME", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(8, 124, buf, COL_GREEN, COL_BG);

    /* Hashrate — demoted on purpose */
    snprintf(buf, sizeof(buf), "%lu H/s", hash_rate);
    quartz_display_draw_text_s(230, 36, buf, COL_GRAY, COL_BG);

    /* Wallet (truncated) */
    quartz_display_draw_text_l(8, 168, "WALLET", COL_WHITE, COL_BG);
    if (wallet_address && strlen(wallet_address) > 34) {
        char short_addr[20];
        memcpy(short_addr, wallet_address, 8);
        short_addr[8] = '.'; short_addr[9] = '.'; short_addr[10] = '.';
        memcpy(short_addr + 11, wallet_address + strlen(wallet_address) - 6, 6);
        short_addr[17] = '\0';
        quartz_display_draw_text_l(8, 188, short_addr, COL_YELLOW, COL_BG);
    } else if (wallet_address) {
        quartz_display_draw_text_l(8, 188, wallet_address, COL_YELLOW, COL_BG);
    }

    /* Button hints */
    quartz_display_fill_rect(0, 220, DISP_W, 20, COL_CARD);
    quartz_display_draw_text_s(8, 223, "[A] Pay", COL_ACCENT, COL_CARD);
    quartz_display_draw_text_s(120, 223, "[B] Next", COL_GRAY, COL_CARD);
    quartz_display_draw_text_s(230, 223, "[C] -", COL_DARKGRAY, COL_CARD);
#endif
}

/* === Fleet Screen ===
 * The shared goal: enrolled devices ticking toward the listing gate.
 */
void quartz_display_fleet_screen(
    uint32_t member_count,
    uint32_t my_shares,
    uint64_t rewards_qz_milli,
    uint32_t blocks_found)
{
#ifdef ESP_PLATFORM
    if (!s_ready) return;

    quartz_display_clear(COL_BG);

    /* Top bar */
    quartz_display_fill_rect(0, 0, DISP_W, 24, COL_ACCENT);
    quartz_display_draw_text_s(8, 4, "QUARTZ", COL_WHITE, COL_ACCENT);
    quartz_display_draw_text_s(DISP_W - 52, 4, "FLEET", COL_CYAN, COL_ACCENT);
    quartz_display_fill_rect(0, 24, DISP_W, 2, COL_ACCENT);

    /* Fleet count — hero, with goal */
    char buf[40];
    quartz_display_draw_text_l(8, 36, "ENROLLED DEVICES", COL_WHITE, COL_BG);
    snprintf(buf, sizeof(buf), "%lu/100", member_count);
    quartz_display_draw_big_text(8, 58, buf, member_count >= 100 ? COL_GREEN : COL_CYAN, COL_BG);

    /* Progress bar toward the goal */
    quartz_display_fill_rect(8, 100, 304, 14, COL_CARD);
    uint32_t bar_w = (member_count * 304) / 100;
    if (bar_w > 304) bar_w = 304;
    if (bar_w > 0) quartz_display_fill_rect(8, 100, bar_w, 14, COL_GREEN);

    /* Stats row */
    quartz_display_draw_text_s(8, 128, "SHARES", COL_GRAY, COL_BG);
    snprintf(buf, sizeof(buf), "%lu", my_shares);
    quartz_display_draw_big_text(8, 142, buf, COL_WHITE, COL_BG);

    quartz_display_draw_text_s(118, 128, "REWARDS", COL_GRAY, COL_BG);
    snprintf(buf, sizeof(buf), "%llu.%02llu",
             rewards_qz_milli / 1000, (rewards_qz_milli % 1000) / 10);
    quartz_display_draw_big_text(118, 142, buf, COL_GREEN, COL_BG);

    quartz_display_draw_text_s(248, 128, "BLOCKS", COL_GRAY, COL_BG);
    snprintf(buf, sizeof(buf), "%lu", blocks_found);
    quartz_display_draw_big_text(248, 142, buf, COL_WHITE, COL_BG);

    /* Note line */
    quartz_display_draw_text_s(8, 190, "100 devices unlocks exchange listings", COL_DARKGRAY, COL_BG);

    /* Button hints */
    quartz_display_fill_rect(0, 220, DISP_W, 20, COL_CARD);
    quartz_display_draw_text_s(8, 223, "[A] Pay", COL_ACCENT, COL_CARD);
    quartz_display_draw_text_s(120, 223, "[B] Next", COL_GRAY, COL_CARD);
    quartz_display_draw_text_s(230, 223, "[C] -", COL_DARKGRAY, COL_CARD);
#endif
}

/* === QR Payment Screen === */
void quartz_display_qr_payment(const char *address, float amount)
{
#ifdef ESP_PLATFORM
    if (!s_ready) return;

    quartz_display_clear(COL_BG);

    /* Header */
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_ACCENT);
    quartz_display_draw_text(8, 3, "SCAN TO PAY", COL_WHITE, COL_ACCENT);
    quartz_display_fill_rect(0, 22, DISP_W, 2, COL_ACCENT);

    /* Build QR string */
    char qr_str[128];
    snprintf(qr_str, sizeof(qr_str), "quartz:%s?amount=%.2f",
             address ? address : "", amount);

    /* Draw QR centered */
    int scale = 3;
    int qr_len = strlen(qr_str);
    int version = quartz_qr_version_for_data(qr_len, QR_ECC_MEDIUM);
    int modules = 21 + 4 * (version - 1);
    int qr_px = modules * scale;
    int qr_x = (DISP_W - qr_px) / 2;
    int qr_y = 28;

    int rc = quartz_qr_display(qr_str, QR_ECC_MEDIUM, qr_x, qr_y, scale,
                               0x0000, 0xFFFF);

    if (rc != 0) {
        quartz_display_draw_text(40, 60, "QR Error", COL_RED, COL_BG);
        quartz_display_draw_text(40, 80, qr_str, COL_WHITE, COL_BG);
    }

    /* Amount below QR */
    int info_y = qr_y + qr_px + 6;
    char amt_str[32];
    snprintf(amt_str, sizeof(amt_str), "%.2f QZ", amount);
    int text_px = strlen(amt_str) * 8;
    quartz_display_draw_big_text((DISP_W - text_px * 2) / 2, info_y,
                                 amt_str, COL_YELLOW, COL_BG);

    /* Button hints */
    quartz_display_fill_rect(0, 220, DISP_W, 20, COL_CARD);
    quartz_display_draw_text(8, 223, "[A] Mine", COL_ACCENT, COL_CARD);
    quartz_display_draw_text(120, 223, "[B] +0.1", COL_GREEN, COL_CARD);
    quartz_display_draw_text(230, 223, "[C] -0.1", COL_RED, COL_CARD);
#endif
}

/* === Connecting === */
void quartz_display_connecting(void) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_draw_big_text(80, 100, "CONNECTING", COL_CYAN, COL_BG);
#endif
}

/* === Error === */
void quartz_display_error(const char *msg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_RED);
    quartz_display_draw_text(8, 100, msg, COL_WHITE, COL_RED);
#endif
}

/* === Seed phrase === */
void quartz_display_seed_phrase(const char words[12][12], int page) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_RED);
    quartz_display_draw_text(8, 3, "WRITE DOWN SEED", COL_WHITE, COL_RED);

    int start = page * 6;
    for (int i = 0; i < 6 && (start + i) < 12; i++) {
        char line[32];
        snprintf(line, sizeof(line), "%d. %s", start + i + 1, words[start + i]);
        quartz_display_draw_big_text(40, 40 + i * 30, line, COL_WHITE, COL_BG);
    }
    if (page == 0) {
        quartz_display_draw_text(80, 220, "[C] Next page", COL_GRAY, COL_BG);
    }
#endif
}

/* === Message === */
void quartz_display_message(const char *from, const char *text, int block_height) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_GREEN);
    quartz_display_draw_text(8, 3, "MESSAGE", COL_WHITE, COL_GREEN);

    char hdr[64];
    snprintf(hdr, sizeof(hdr), "From: %s  Block: %d", from, block_height);
    quartz_display_draw_text(8, 32, hdr, COL_CYAN, COL_BG);
    quartz_display_draw_text(8, 56, text, COL_WHITE, COL_BG);
#endif
}

/* === Portal === */
void quartz_display_portal(const char *ap_name) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_ACCENT);
    quartz_display_draw_text(8, 3, "WIFI SETUP", COL_WHITE, COL_ACCENT);
    quartz_display_fill_rect(0, 22, DISP_W, 2, COL_ACCENT);

    quartz_display_draw_text(8, 36, "1. Connect phone to:", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(8, 52, ap_name, COL_CYAN, COL_BG);

    quartz_display_draw_text(8, 84, "2. Open browser:", COL_WHITE, COL_BG);
    quartz_display_draw_big_text(8, 100, "192.168.4.1", COL_YELLOW, COL_BG);

    quartz_display_draw_text(8, 132, "3. Enter WiFi password", COL_WHITE, COL_BG);

    /* Animated dots */
    uint32_t t = esp_timer_get_time() / 1000;
    int dots = (t / 500) % 4;
    char dotbuf[8] = {0};
    for (int i = 0; i < dots; i++) dotbuf[i] = '.';
    quartz_display_draw_big_text(8, 160, dotbuf, COL_ACCENT, COL_BG);
#endif
}

/* === PIN Entry Screen === */

static char s_pin_display[9] = {0};  /* current PIN digits entered */
static int s_pin_len = 0;
static int s_pin_digit = 0;          /* current digit being edited (0-9) */
static int s_pin_attempts_display = 10;

void quartz_display_pin_entry(const char *entered_pin, int attempts_left) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);

    /* Header */
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_RED);
    quartz_display_draw_text(8, 3, "PIN REQUIRED", COL_WHITE, COL_RED);

    /* PIN dots / digits */
    int pin_len = entered_pin ? strlen(entered_pin) : 0;
    char dots[16] = {0};
    int dx = 0;
    for (int i = 0; i < 8; i++) {
        if (i < pin_len) {
            dots[dx++] = '\xE2'; dots[dx++] = '\x80'; dots[dx++] = '\xA2'; /* bullet • */
        } else {
            dots[dx++] = '_';
        }
        dots[dx++] = ' ';
    }
    dots[dx] = '\0';
    quartz_display_draw_big_text(40, 40, dots, COL_WHITE, COL_BG);

    /* Instructions */
    quartz_display_draw_text(8, 80, "Serial: 'pin <digits>'", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 96, "BLE: unlock via app", COL_GRAY, COL_BG);

    /* Attempts */
    char buf[32];
    snprintf(buf, sizeof(buf), "Attempts left: %d", attempts_left);
    quartz_display_draw_text(8, 130, buf,
                             attempts_left <= 3 ? COL_RED : COL_GRAY, COL_BG);

    /* Buttons hint */
    quartz_display_draw_text(8, 200, "A:up B:next C:confirm", COL_DARKGRAY, COL_BG);
#endif
}

void quartz_display_pin_entry_m5stack(int digit, int pin_len, int attempts_left) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);

    /* Header */
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_RED);
    quartz_display_fill_rect(0, 22, DISP_W, 2, COL_RED);

    /* Title */
    quartz_display_draw_text_l(60, 30, "ENTER PIN", COL_WHITE, COL_BG);

    /* Big current digit */
    char dbuf[2] = {0};
    dbuf[0] = '0' + (digit % 10);
    quartz_display_draw_huge_text(130, 60, dbuf, COL_ACCENT, COL_BG);

    /* PIN dots */
    char dots[24] = {0};
    int dx = 0;
    for (int i = 0; i < 8; i++) {
        if (i < pin_len) {
            dots[dx++] = '\xE2'; dots[dx++] = '\x80'; dots[dx++] = '\xA2';
        } else if (i == pin_len) {
            dots[dx++] = '[';
        } else {
            dots[dx++] = '_';
        }
        dots[dx++] = ' ';
        if (i == pin_len) dots[dx++] = ']';
    }
    dots[dx] = '\0';
    quartz_display_draw_big_text(40, 160, dots, COL_WHITE, COL_BG);

    /* Buttons */
    quartz_display_draw_text(8, 200, "A:digit+  B:next   C:OK", COL_GRAY, COL_BG);

    /* Attempts */
    if (attempts_left < 10) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d left", attempts_left);
        quartz_display_draw_text(260, 200, buf,
                                 attempts_left <= 3 ? COL_RED : COL_GRAY, COL_BG);
    }
#endif
}

/* === Recovery Screen === */

void quartz_display_recovery_start(void) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_ACCENT);
    quartz_display_draw_text(8, 3, "RECOVERY MODE", COL_WHITE, COL_ACCENT);

    quartz_display_draw_text_l(20, 40, "Enter Seed Phrase", COL_WHITE, COL_BG);
    quartz_display_draw_text(8, 70, "Serial: 'recover word1 word2 ...'", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 86, "BLE: use app recovery screen", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 120, "Waiting for input...", COL_YELLOW, COL_BG);
#endif
}

void quartz_display_recovery_sync(const char *address) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_ACCENT);
    quartz_display_draw_text(8, 3, "SYNCING...", COL_WHITE, COL_ACCENT);

    quartz_display_draw_text(8, 40, "Address:", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 56, address, COL_CYAN, COL_BG);

    /* Animated dots */
    uint32_t t = esp_timer_get_time() / 1000;
    int dots = (t / 500) % 4;
    char dotbuf[8] = {0};
    for (int i = 0; i < dots; i++) dotbuf[i] = '.';
    quartz_display_draw_big_text(8, 90, dotbuf, COL_ACCENT, COL_BG);

    quartz_display_draw_text(8, 130, "Querying node for", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 146, "signature index...", COL_GRAY, COL_BG);
#endif
}

void quartz_display_recovery_done(const char *address, int sig_index, int balance) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_GREEN);
    quartz_display_draw_text(8, 3, "RECOVERED!", COL_WHITE, COL_GREEN);

    quartz_display_draw_text(8, 40, "Address:", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 56, address, COL_CYAN, COL_BG);

    char buf[64];
    snprintf(buf, sizeof(buf), "Signatures used: %d/255", sig_index);
    quartz_display_draw_text(8, 80, buf, COL_WHITE, COL_BG);

    snprintf(buf, sizeof(buf), "Balance: %d QZ", balance);
    quartz_display_draw_text(8, 100, buf, COL_GREEN, COL_BG);

    quartz_display_draw_text(8, 140, "Mining will resume shortly.", COL_ACCENT, COL_BG);
#endif
}

void quartz_display_recovery_error(const char *msg) {
#ifdef ESP_PLATFORM
    if (!s_ready) return;
    quartz_display_clear(COL_BG);
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_RED);
    quartz_display_draw_text(8, 3, "RECOVERY FAILED", COL_WHITE, COL_RED);

    quartz_display_draw_text(8, 40, msg, COL_YELLOW, COL_BG);
    quartz_display_draw_text(8, 80, "Check seed phrase and", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 96, "node connection.", COL_GRAY, COL_BG);
#endif
}
