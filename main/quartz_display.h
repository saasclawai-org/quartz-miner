/**
 * quartz_display.h — M5Stack Core (ILI9341) Display Driver for Quartz
 *
 * Shows wallet info, mining stats, and seed phrase on the TFT.
 */

#ifndef QUARTZ_DISPLAY_H
#define QUARTZ_DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Screen dimensions */
#define QZ_SCREEN_W   320
#define QZ_SCREEN_H   240

/* Colors (RGB565) */
#define QZ_COLOR_BLACK       0x0000
#define QZ_COLOR_WHITE       0xFFFF
#define QZ_COLOR_PURPLE      0x9933  /* Quartz brand purple */
#define QZ_COLOR_CYAN        0x07FF
#define QZ_COLOR_GREEN       0x07E0
#define QZ_COLOR_RED         0xF800
#define QZ_COLOR_YELLOW      0xFFE0
#define QZ_COLOR_ORANGE      0xFD20
#define QZ_COLOR_DARK_BG     0x18E3  /* Dark blue-purple */
#define QZ_COLOR_GRAY        0x8410
#define QZ_COLOR_DARKGRAY    0x4208

/* Initialize the display (SPI + ILI9341 config) */
void quartz_display_init(void);

/* Clear screen with color */
void quartz_display_clear(uint16_t color);

/* Fill a rectangle */
void quartz_display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draw a single pixel */
void quartz_display_draw_pixel(int x, int y, uint16_t color);

/* Draw text at position. Font is 8x16, returns new Y after text. */
void quartz_display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw larger text (16x32 bold digits for stats) */
void quartz_display_draw_big_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw a horizontal line */
void quartz_display_draw_hline(int x, int y, int w, uint16_t color);

/* ===== High-level Quartz screens ===== */

/* Splash screen with Quartz logo */
void quartz_display_splash(void);

/* Mining dashboard: hashrate, blocks, uptime, address */
void quartz_display_mining_stats(
    uint32_t hash_count,
    uint32_t hash_rate,
    uint32_t blocks_found,
    uint32_t uptime_seconds,
    const char *wallet_address
);

/* Seed phrase display (first boot).
 * Shows words 1-6 on first call, 7-12 on second.
 * page: 0 or 1 */
void quartz_display_seed_phrase(const char words[12][12], int page);

/* Waiting / connecting screen */
void quartz_display_connecting(void);

/* Error screen */
void quartz_display_error(const char *msg);

/* Message display (latest message from the chain) */
void quartz_display_message(const char *from, const char *text, int block_height);

/* WiFi portal setup screen */
void quartz_display_portal(const char *ap_name);

/* QR Payment screen — shows QR code for receiving QZ */
void quartz_display_qr_payment(const char *address, float amount);

/* Screen navigation */
typedef enum {
    QZ_SCREEN_MINING = 0,
    QZ_SCREEN_WALLET = 1,
    QZ_SCREEN_MESSAGES = 2,
    QZ_SCREEN_PAYMENT = 3,
    QZ_SCREEN_COUNT,
} qz_screen_t;

void quartz_display_set_screen(qz_screen_t screen);
qz_screen_t quartz_display_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_DISPLAY_H
