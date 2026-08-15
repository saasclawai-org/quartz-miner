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

/* Identity screen — boot default. Hero: enrollment status + device ID.
 * Hashrate intentionally demoted to a small line. */
void quartz_display_id_screen(
    bool enrolled,
    const uint8_t device_id[32],
    uint32_t uptime_sec,
    uint32_t hash_rate,
    const char *wallet_address);

/* Fleet screen — network progress toward the 100-device goal. */
void quartz_display_fleet_screen(
    uint32_t member_count,
    uint32_t my_shares,
    uint64_t rewards_qz_milli,
    uint32_t blocks_found);

/* Fill a rectangle */
void quartz_display_fill_rect(int x, int y, int w, int h, uint16_t color);

/* Draw a single pixel */
void quartz_display_draw_pixel(int x, int y, uint16_t color);

/* Draw text at position. Font is 8x16, returns new Y after text. */
void quartz_display_draw_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw larger text (16x32 bold digits for stats) */
void quartz_display_draw_big_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw huge text (32x64 for titles) */
void quartz_display_draw_huge_text(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw text with DejaVu Sans 12px (small, clean) */
void quartz_display_draw_text_s(int x, int y, const char *text, uint16_t fg, uint16_t bg);

/* Draw text with DejaVu Sans 16px (large, clean) */
void quartz_display_draw_text_l(int x, int y, const char *text, uint16_t fg, uint16_t bg);

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
    QZ_SCREEN_ID = 0,          /* Identity — boot default */
    QZ_SCREEN_MINING = 1,
    QZ_SCREEN_FLEET = 2,       /* Fleet / network progress */
    QZ_SCREEN_WALLET = 3,
    QZ_SCREEN_MESSAGES = 4,
    QZ_SCREEN_PAYMENT = 5,
    QZ_SCREEN_PIN_ENTRY = 6,
    QZ_SCREEN_RECOVERY = 7,
    QZ_SCREEN_COUNT,
} qz_screen_t;

void quartz_display_set_screen(qz_screen_t screen);
qz_screen_t quartz_display_get_screen(void);

/* PIN entry screen — shows entered PIN as dots, attempts left */
void quartz_display_pin_entry(const char *entered_pin, int attempts_left);

/* PIN entry M5Stack mode — big digit, 3-button navigation */
void quartz_display_pin_entry_m5stack(int digit, int pin_len, int attempts_left);

/* Recovery screens */
void quartz_display_recovery_start(void);
void quartz_display_recovery_sync(const char *address);
void quartz_display_recovery_done(const char *address, int sig_index, int balance);
void quartz_display_recovery_error(const char *msg);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_DISPLAY_H
