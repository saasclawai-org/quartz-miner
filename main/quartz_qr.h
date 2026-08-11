/**
 * quartz_qr.h — Minimal QR Code Generator for ESP32
 *
 * Generates QR codes directly on the ILI9341 display.
 * Supports versions 1-10 (enough for payment URLs).
 * Error correction level: M (15% recovery)
 *
 * Based on the QR code ISO/IEC 18004 standard.
 * Compact implementation optimized for embedded use.
 */

#ifndef QUARTZ_QR_H
#define QUARTZ_QR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* QR error correction levels */
typedef enum {
    QR_ECC_LOW = 0,      /* 7% recovery */
    QR_ECC_MEDIUM = 1,   /* 15% recovery */
    QR_ECC_QUARTILE = 2, /* 25% recovery */
    QR_ECC_HIGH = 3      /* 30% recovery */
} qr_ecc_t;

/* QR version 1-10 module counts */
#define QR_MAX_MODULES 57  /* version 10 = 57x57 */

/**
 * Render a QR code on the ILI9341 display.
 *
 * @param data      String to encode
 * @param ecc       Error correction level
 * @param x         Top-left X position on screen
 * @param y         Top-left Y position on screen
 * @param scale     Pixels per module (1-4)
 * @param fg_color  Foreground (modules) color (RGB565)
 * @param bg_color  Background color (RGB565)
 * @return 0 on success, -1 on error (data too long)
 */
int quartz_qr_display(
    const char *data,
    qr_ecc_t ecc,
    int x, int y,
    int scale,
    uint16_t fg_color,
    uint16_t bg_color
);

/**
 * Get the QR version needed for a given data length.
 */
int quartz_qr_version_for_data(int data_len, qr_ecc_t ecc);

#ifdef __cplusplus
}
#endif

#endif /* QUARTZ_QR_H */
