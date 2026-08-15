/**
 * quartz_gui.c — uGUI integration for Quartz M5Stack display
 * Bridges uGUI library to our ILI9341 SPI driver
 */

#include "ugui.h"
#include "font_dejavu14.h"
#include "font_dejavu18.h"
#include "font_dejavu24.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include <string.h>

static const char *TAG = "QZ.GUI";

/* M5Stack display pins */
#define PIN_MOSI   23
#define PIN_CLK    18
#define PIN_CS     14
#define PIN_DC     27
#define PIN_RST    33
#define PIN_BL     32

#define DISP_W  320
#define DISP_H  240

static spi_device_handle_t s_spi = NULL;
static UG_GUI s_gui;
static bool s_init = false;

/* ----- SPI helpers (from working display.c) ----- */
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
    spi_cmd(0x2A);
    uint8_t cx[4] = {(x1>>8)&0xFF, x1&0xFF, (x2>>8)&0xFF, x2&0xFF};
    spi_data(cx, 4);
    spi_cmd(0x2B);
    uint8_t ry[4] = {(y1>>8)&0xFF, y1&0xFF, (y2>>8)&0xFF, y2&0xFF};
    spi_data(ry, 4);
    spi_cmd(0x2C);
}

/* ----- uGUI callbacks ----- */
static void quartz_ugui_pset(UG_S16 x, UG_S16 y, UG_COLOR c) {
    if (x < 0 || x >= DISP_W || y < 0 || y >= DISP_H) return;
    
    /* Draw single pixel */
    set_addr_window(x, y, x, y);
    gpio_set_level(PIN_DC, 1);
    uint16_t color = c;
    spi_transaction_t t = {0};
    t.length = 16;
    t.tx_buffer = &color;
    spi_device_polling_transmit(s_spi, &t);
}

/* Buffer-based fill for better performance (uGUI driver callback) */
static UG_RESULT quartz_ugui_fill(UG_S16 x1, UG_S16 y1, UG_S16 x2, UG_S16 y2, UG_COLOR c) {
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 >= DISP_W) x2 = DISP_W - 1;
    if (y2 >= DISP_H) y2 = DISP_H - 1;
    if (x1 > x2 || y1 > y2) return UG_RESULT_FAIL;
    
    int w = x2 - x1 + 1;
    int h = y2 - y1 + 1;
    int total = w * h;
    int chunk = 1024;
    uint16_t *buf = heap_caps_malloc(chunk * 2, MALLOC_CAP_8BIT);
    if (!buf) return UG_RESULT_FAIL;
    for (int i = 0; i < chunk; i++) buf[i] = c;
    
    set_addr_window(x1, y1, x2, y2);
    gpio_set_level(PIN_DC, 1);
    
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
    return UG_RESULT_OK;
}

/* ----- Init ----- */
void quartz_gui_init(void) {
    if (s_init) return;
    
    ESP_LOGI(TAG, "Initializing uGUI + ILI9341");
    
    /* GPIO */
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
    buscfg.miso_io_num = -1;
    buscfg.sclk_io_num = PIN_CLK;
    buscfg.max_transfer_sz = DISP_W * DISP_H * 2 + 8;
    spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
    
    spi_device_interface_config_t devcfg = {0};
    devcfg.clock_speed_hz = 27 * 1000 * 1000;
    devcfg.mode = 0;
    devcfg.spics_io_num = PIN_CS;
    devcfg.queue_size = 7;
    spi_bus_add_device(SPI2_HOST, &devcfg, &s_spi);
    
    /* Hardware reset */
    gpio_set_level(PIN_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(PIN_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));
    
    /* ILI9341 init (M5Stack Arduino sequence) */
    spi_cmd(0xCF); spi_data1(0x00); spi_data1(0xC1); spi_data1(0x30);
    spi_cmd(0xED); spi_data1(0x64); spi_data1(0x03); spi_data1(0x12); spi_data1(0x81);
    spi_cmd(0xE8); spi_data1(0x85); spi_data1(0x00); spi_data1(0x78);
    spi_cmd(0xCB); spi_data1(0x39); spi_data1(0x2C); spi_data1(0x00);
    spi_data1(0x34); spi_data1(0x02);
    spi_cmd(0xF7); spi_data1(0x20);
    spi_cmd(0xEA); spi_data1(0x00); spi_data1(0x00);
    spi_cmd(0xC0); spi_data1(0x23);
    spi_cmd(0xC1); spi_data1(0x10);
    spi_cmd(0xC5); spi_data1(0x3E); spi_data1(0x28);
    spi_cmd(0xC7); spi_data1(0x86);
    spi_cmd(0x36); spi_data1(0x60);  /* MADCTL: landscape, RGB */
    spi_cmd(0x3A); spi_data1(0x55);
    spi_cmd(0xB1); spi_data1(0x00); spi_data1(0x18);
    spi_cmd(0xB6); spi_data1(0x08); spi_data1(0x82); spi_data1(0x27);
    spi_cmd(0xF2); spi_data1(0x00);
    spi_cmd(0x26); spi_data1(0x01);
    
    /* Gamma */
    spi_cmd(0xE0);
    uint8_t pgamma[] = {0x0F,0x31,0x2B,0x0C,0x0E,0x08,0x4E,0xF1,0x37,0x07,0x10,0x03,0x0E,0x09,0x00};
    spi_data(pgamma, 15);
    spi_cmd(0xE1);
    uint8_t ngamma[] = {0x00,0x0E,0x14,0x03,0x11,0x07,0x31,0xC1,0x48,0x08,0x0F,0x0C,0x31,0x36,0x0F};
    spi_data(ngamma, 15);
    
    spi_cmd(0x11);
    vTaskDelay(pdMS_TO_TICKS(120));
    spi_cmd(0x29);
    vTaskDelay(pdMS_TO_TICKS(50));
    
    gpio_set_level(PIN_BL, 1);
    
    /* Init uGUI */
    UG_Init(&s_gui, quartz_ugui_pset, DISP_W, DISP_H);
    UG_SelectGUI(&s_gui);
    
    /* Register accelerated fill driver for fast rectangle fills */
    UG_DriverRegister(DRIVER_FILL_FRAME, (void*)quartz_ugui_fill);
    
    /* Default font */
    UG_FontSelect(&FONT_DEJAVU14);
    
    s_init = true;
    ESP_LOGI(TAG, "uGUI ready");
}

/* Convenience: clear screen with color */
void quartz_gui_clear(UG_COLOR c) {
    quartz_ugui_fill(0, 0, DISP_W - 1, DISP_H - 1, c);
}

/* Get the UG_GUI handle for direct uGUI calls */
UG_GUI* quartz_gui_get(void) {
    return &s_gui;
}

#endif /* ESP_PLATFORM */