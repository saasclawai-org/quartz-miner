/**
 * quartz_display.c — M5Stack Core ILI9341 Display Driver
 *
 * Clean rewrite. Three screens: splash → mining → QR payment.
 * Pins: MOSI=23, CLK=18, CS=14, DC=27, RST=33, BL=32
 */

#include "quartz_display.h"
#include "quartz_qr.h"
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

/* ============================================================
 * 8x16 Font (ASCII 32-126)
 * Each char is 16 bytes, one per row. Bit MSB = leftmost pixel.
 * ============================================================ */
static const uint8_t font8x16[][16] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x36,0x36,0x7F,0x36,0x36,0x36,0x7F,0x36,0x36,0x36,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x3C,0x7E,0x60,0x3C,0x06,0x7E,0x3C,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x33,0x18,0x0C,0x66,0xC6,0xC6,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x36,0x36,0x1C,0x3B,0x6E,0x66,0x3E,0x06,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x30,0x18,0x0C,0x0C,0x0C,0x0C,0x0C,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x38,0x78,0x18,0x18,0x18,0x18,0x18,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x30,0x60,0x66,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x06,0x06,0x1C,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0C,0x1C,0x3C,0x6C,0x7E,0x0C,0x0C,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x60,0x7C,0x06,0x06,0x06,0x06,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1C,0x30,0x60,0x7C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x30,0x30,0x30,0x30,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x3C,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x3E,0x06,0x06,0x0C,0x38,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x0C,0x0C,0x00,0x00,0x0C,0x0C,0x18,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x06,0x0C,0x18,0x18,0x00,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x6E,0x6E,0x6E,0x62,0x60,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x66,0x66,0x66,0x66,0x7C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x60,0x60,0x60,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x78,0x6C,0x66,0x66,0x66,0x66,0x66,0x6C,0x78,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x60,0x60,0x6E,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x7E,0x66,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x1E,0x0C,0x0C,0x0C,0x0C,0x0C,0x6C,0x6C,0x38,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x6C,0x78,0x70,0x78,0x6C,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x60,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x77,0x7F,0x6B,0x63,0x63,0x63,0x63,0x63,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x76,0x7E,0x7E,0x6E,0x66,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x60,0x60,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x66,0x66,0x66,0x66,0x7C,0x6C,0x36,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7C,0x66,0x66,0x7C,0x78,0x6C,0x66,0x66,0x66,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x3C,0x66,0x60,0x3C,0x06,0x06,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x66,0x66,0x66,0x66,0x3C,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x63,0x6B,0x6B,0x7F,0x77,0x63,0x63,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x63,0x63,0x36,0x1C,0x1C,0x36,0x63,0x63,0x63,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x66,0x66,0x66,0x3C,0x18,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x7E,0x06,0x0C,0x18,0x30,0x60,0x40,0x00,0x7E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x0E,0x18,0x18,0x18,0x70,0x18,0x18,0x18,0x0E,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x18,0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x18,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x70,0x18,0x18,0x18,0x0E,0x18,0x18,0x18,0x70,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x00,0x00,0x10,0x38,0x6C,0xC6,0xC6,0xFE,0x00,0x00,0x00,0x00,0x00,0x00},
};

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
    ESP_LOGI(TAG, "Init ILI9341...");

    /* GPIO: backlight, DC, RST, CS */
    gpio_config_t io = {0};
    io.pin_bit_mask = (1ULL<<PIN_BL) | (1ULL<<PIN_DC) | (1ULL<<PIN_RST) | (1ULL<<PIN_CS);
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);

    gpio_set_level(PIN_CS, 1);
    gpio_set_level(PIN_DC, 0);
    gpio_set_level(PIN_BL, 0);
    gpio_set_level(PIN_RST, 1);

    /* SPI bus */
    spi_bus_config_t buscfg = {0};
    buscfg.mosi_io_num = PIN_MOSI;
    buscfg.sclk_io_num = PIN_CLK;
    buscfg.max_transfer_sz = DISP_W * DISP_H * 2 + 8;
    spi_bus_initialize(HSPI_HOST, &buscfg, SPI_DMA_CH_AUTO);

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 16 * 1000 * 1000,
        .mode = 0,
        .spics_io_num = PIN_CS,
        .queue_size = 7,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };
    spi_bus_add_device(HSPI_HOST, &devcfg, &s_spi);

    /* Hardware reset */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* ILI9341 init */
    spi_cmd(0x01); vTaskDelay(pdMS_TO_TICKS(100));   /* Reset */
    spi_cmd(0x11); vTaskDelay(pdMS_TO_TICKS(150));   /* Sleep out */
    spi_cmd(0x3A); spi_data1(0x55);                   /* 16-bit color */
    /* MADCTL — cycle through all 8 orientations with BTN C during splash.
     * Default starts at 0x28. Saved to NVS once user picks one.
     * M5Stack V2.7 IPS needs specific value we'll lock in. */
    static const uint8_t madctl_options[8] = {0x28, 0xE8, 0x48, 0x88, 0x68, 0xA8, 0x08, 0xC8};
    int madctl_idx = 0;

    /* Try loading from NVS first */
    uint8_t madctl = 0x28;
    {
        nvs_handle_t h;
        if (nvs_open("qz_disp", NVS_READONLY, &h) == ESP_OK) {
            uint8_t saved = 0;
            if (nvs_get_u8(h, "madctl", &saved) == ESP_OK) {
                madctl = saved;
                ESP_LOGI(TAG, "Loaded MADCTL 0x%02X from NVS", madctl);
            }
            nvs_close(h);
        }
    }

    /* Find index in options */
    for (int i = 0; i < 8; i++) {
        if (madctl_options[i] == madctl) { madctl_idx = i; break; }
    }

    spi_cmd(0x36); spi_data1(madctl);
    ESP_LOGI(TAG, "MADCTL = 0x%02X (idx %d)", madctl, madctl_idx);

    /* Show orientation picker on splash */
    s_ready = true;
    quartz_display_clear(0x18E3);
    quartz_display_fill_rect(0, 0, 320, 30, 0x9933);
    quartz_display_draw_big_text(70, 4, "QUARTZ", 0xFFFF, 0x9933);

    char madctls[32];
    snprintf(madctls, sizeof(madctls), "MADCTL: 0x%02X", madctl);
    quartz_display_draw_big_text(90, 50, madctls, 0x07FF, 0x18E3);
    quartz_display_draw_text(60, 80, "Press BTN C to cycle orientation", 0x8410, 0x18E3);
    quartz_display_draw_text(80, 100, "Press BTN A when it looks right", 0x8410, 0x18E3);

    /* Wait for user to pick orientation */
    bool picked = false;
    while (!picked) {
        vTaskDelay(pdMS_TO_TICKS(50));

        bool btn_c = (gpio_get_level(37) == 0);  /* BTN C */
        bool btn_a = (gpio_get_level(39) == 0);  /* BTN A */

        static bool c_held = false;
        static bool a_held = false;

        if (btn_c && !c_held) {
            c_held = true;
            madctl_idx = (madctl_idx + 1) % 8;
            madctl = madctl_options[madctl_idx];
            spi_cmd(0x36); spi_data1(madctl);

            /* Redraw screen with new orientation */
            quartz_display_clear(0x18E3);
            quartz_display_fill_rect(0, 0, 320, 30, 0x9933);
            quartz_display_draw_big_text(70, 4, "QUARTZ", 0xFFFF, 0x9933);
            snprintf(madctls, sizeof(madctls), "MADCTL: 0x%02X", madctl);
            quartz_display_draw_big_text(90, 50, madctls, 0x07FF, 0x18E3);
            quartz_display_draw_text(60, 80, "Press BTN C to cycle orientation", 0x8410, 0x18E3);
            quartz_display_draw_text(80, 100, "Press BTN A when it looks right", 0x8410, 0x18E3);

            ESP_LOGI(TAG, "MADCTL -> 0x%02X", madctl);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (!btn_c) c_held = false;

        if (btn_a && !a_held) {
            a_held = true;
            picked = true;
            /* Save to NVS */
            nvs_handle_t h;
            if (nvs_open("qz_disp", NVS_READWRITE, &h) == ESP_OK) {
                nvs_set_u8(h, "madctl", madctl);
                nvs_commit(h);
                nvs_close(h);
            }
            ESP_LOGI(TAG, "MADCTL saved: 0x%02X", madctl);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        if (!btn_a) a_held = false;
    }
    spi_cmd(0xC0); spi_data1(0x23);
    spi_cmd(0xC1); spi_data1(0x10);
    spi_cmd(0xC5); spi_data1(0x3E); spi_data1(0x28);
    spi_cmd(0xC7); spi_data1(0x86);
    spi_cmd(0x29); vTaskDelay(pdMS_TO_TICKS(50));    /* Display ON */

    gpio_set_level(PIN_BL, 1);                        /* Backlight on */
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

/* === Splash Screen — now just clears after orientation pick === */
void quartz_display_splash(void) {
    /* Orientation selection happens in quartz_display_init() before this.
     * Just show a brief confirm flash here. */
#ifdef ESP_PLATFORM
    vTaskDelay(pdMS_TO_TICKS(500));
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
    quartz_display_fill_rect(0, 0, DISP_W, 22, COL_ACCENT);
    quartz_display_draw_text(8, 3, "QUARTZ MINER", COL_WHITE, COL_ACCENT);

    /* Connection status (top right) */
    quartz_display_draw_text(DISP_W - 52, 3, "SOLO", COL_YELLOW, COL_ACCENT);

    /* Separator */
    quartz_display_fill_rect(0, 22, DISP_W, 2, COL_ACCENT);

    /* Hashrate — big number */
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", hash_rate);
    quartz_display_draw_text(8, 32, "HASHRATE", COL_GRAY, COL_BG);
    quartz_display_draw_big_text(8, 48, buf, COL_CYAN, COL_BG);
    int hlen = strlen(buf) * 16;
    quartz_display_draw_big_text(8 + hlen, 48, " H/s", COL_GRAY, COL_BG);

    /* Blocks found */
    snprintf(buf, sizeof(buf), "%lu", blocks_found);
    quartz_display_draw_text(8, 84, "BLOCKS", COL_GRAY, COL_BG);
    quartz_display_draw_big_text(8, 100, buf, COL_GREEN, COL_BG);

    /* Uptime */
    uint32_t hrs = uptime_sec / 3600;
    uint32_t mins = (uptime_sec % 3600) / 60;
    snprintf(buf, sizeof(buf), "%luh %lum", hrs, mins);
    quartz_display_draw_text(170, 84, "UPTIME", COL_GRAY, COL_BG);
    quartz_display_draw_big_text(170, 100, buf, COL_WHITE, COL_BG);

    /* Total hashes */
    snprintf(buf, sizeof(buf), "%lu", hash_count);
    quartz_display_draw_text(8, 136, "TOTAL HASHES", COL_GRAY, COL_BG);
    quartz_display_draw_text(8, 152, buf, COL_WHITE, COL_BG);

    /* Wallet address (truncated) */
    quartz_display_draw_text(8, 176, "WALLET", COL_GRAY, COL_BG);
    if (wallet_address && strlen(wallet_address) > 34) {
        char short_addr[20];
        memcpy(short_addr, wallet_address, 8);
        short_addr[8] = '.'; short_addr[9] = '.'; short_addr[10] = '.';
        memcpy(short_addr + 11, wallet_address + strlen(wallet_address) - 6, 6);
        short_addr[17] = '\0';
        quartz_display_draw_text(8, 192, short_addr, COL_YELLOW, COL_BG);
    } else if (wallet_address) {
        quartz_display_draw_text(8, 192, wallet_address, COL_YELLOW, COL_BG);
    }

    /* Button hints at bottom */
    quartz_display_fill_rect(0, 220, DISP_W, 20, COL_CARD);
    quartz_display_draw_text(8, 223, "[A] Pay", COL_ACCENT, COL_CARD);
    quartz_display_draw_text(120, 223, "[B] Wallet", COL_GRAY, COL_CARD);
    quartz_display_draw_text(230, 223, "[C] Msgs", COL_GRAY, COL_CARD);
#endif
}

/* === QR Payment Screen === */
void quartz_display_qr_payment(const char *address, float amount) {
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
