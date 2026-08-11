/**
 * quartz_pay.h — QR Payments + Relay Control
 *
 * Turn any ESP32 into a crypto point-of-sale:
 * 1. Display QR code with payment request (address + amount)
 * 2. Phone scans, sends QZ to device wallet
 * 3. Device polls node for incoming transactions
 * 4. On confirmation, trigger GPIO relay (door, vending, etc.)
 *
 * QR Format: quartz:<address>?amount=<qz>&label=<text>
 * (BIP-21 style, like Bitcoin's bitcoin: URI scheme)
 */

#ifndef QUARTZ_PAY_H
#define QUARTZ_PAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* === Configuration === */

/* Relay GPIO pin (M5Stack Core: M5.Port B = GPIO36, or use internal pins) */
#define QZ_PAY_RELAY_PIN        26      /* GPIO26 — M5Stack side port */
#define QZ_PAY_RELAY_ACTIVE_LOW  false   /* most relay modules are active-high */
#define QZ_PAY_RELAY_DURATION_MS 3000    /* how long to trigger relay (0 = toggle) */

/* Payment polling */
#define QZ_PAY_POLL_INTERVAL_S   5       /* check for payments every 5 seconds */
#define QZ_PAY_TIMEOUT_S         300     /* payment request expires after 5 minutes */
#define QZ_PAY_CONFIRMATIONS     1       /* blocks needed for confirmation */

/* QR code sizing */
#define QZ_QR_VERSION            5       /* QR version 5 = 84x84 modules (fits 320x240) */
#define QZ_QR_SCALE              2       /* pixels per module */
#define QZ_QR_BORDER             4       /* quiet zone modules */

/* === Types === */

typedef enum {
    QZ_PAY_IDLE = 0,
    QZ_PAY_WAITING,       /* QR displayed, waiting for payment */
    QZ_PAY_RECEIVED,      /* payment detected, confirming */
    QZ_PAY_CONFIRMED,     /* confirmed, triggering relay */
    QZ_PAY_EXPIRED,       /* timeout */
    QZ_PAY_ERROR
} qz_pay_state_t;

typedef struct {
    qz_pay_state_t state;
    char address[65];         /* wallet address (hex) */
    uint64_t amount_satoshis; /* requested amount in quartz-sats */
    char label[33];           /* optional label for the QR code */
    uint32_t created_time;    /* when payment request was created */
    uint32_t expires_time;    /* when it expires */
    char tx_hash[65];         /* hash of received payment tx */
    uint32_t relay_trigger_time; /* when relay was activated */
} qz_pay_request_t;

/* === API === */

/**
 * Initialize payment system.
 * Sets up relay GPIO, loads wallet address.
 */
int quartz_pay_init(const char *wallet_address);

/**
 * Create a payment request.
 * Generates QR data, displays on screen, starts polling.
 *
 * @param amount_qz   Amount in QZ (e.g., 0.50 for half a QZ)
 * @param label       Optional label text (max 32 chars)
 * @return 0 on success
 */
int quartz_pay_request(float amount_qz, const char *label);

/**
 * Poll for incoming payment.
 * Called from main loop. Checks node API for transactions to our address.
 *
 * @return QZ_PAY_RECEIVED if payment detected, QZ_PAY_CONFIRMED if confirmed
 */
qz_pay_state_t quartz_pay_poll(void);

/**
 * Trigger the relay (GPIO).
 * Called automatically on payment confirmation, or manually.
 *
 * @param duration_ms  How long to activate (0 = use default)
 */
void quartz_pay_trigger_relay(uint32_t duration_ms);

/**
 * Cancel current payment request.
 */
void quartz_pay_cancel(void);

/**
 * Get current payment state.
 */
qz_pay_state_t quartz_pay_get_state(void);

/**
 * Get current payment request info.
 */
const qz_pay_request_t *quartz_pay_get_request(void);

/**
 * Build QR code string for display.
 * Format: quartz:<address>?amount=<QZ>&label=<text>
 *
 * @param buf      Output buffer
 * @param buf_len  Buffer size
 * @param address  Wallet address
 * @param amount_qz Amount in QZ
 * @param label    Optional label
 * @return String length, or -1 on error
 */
int quartz_pay_build_qr_string(
    char *buf, int buf_len,
    const char *address,
    float amount_qz,
    const char *label
);

#ifdef __cplusplus
}
#endif

#endif /* QUARTZ_PAY_H */
