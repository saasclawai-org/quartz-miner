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
#include <stdbool.h>
#include <stddef.h>   /* v089.12: size_t for relay_cmd/build_relay_json */

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
#define QZ_PAY_TIMEOUT_S         86400  /* v083: watch expires after 24h (auto mode: never) */
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
    char known_txids[6][17];  /* v081: all txids seen at arm time — only NEW payments fire */
    int  known_txid_count;
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
/* v079: runtime overrides (NVS-persisted, applied at init) */
void quartz_pay_set_duration_ms(uint32_t duration_ms);
uint32_t quartz_pay_get_duration_ms(void);
uint8_t quartz_pay_get_pin(void);
/* v080: 0-conf fire mode + relay polarity invert (NVS-persisted) */
void quartz_pay_set_fast(bool fast);
bool quartz_pay_get_fast(void);
void quartz_pay_toggle_invert(void);
bool quartz_pay_get_invert(void);
/* v089.12: absolute polarity (BLE sets state; CLI keeps bare toggle) */
void quartz_pay_set_invert(bool invert);
/* v082: auto re-arm (vending mode) — after firing, re-request the same
 * amount with a fresh arm snapshot. Coinbase txs (counterparty null —
 * mining rewards) never fire the relay. */
void quartz_pay_set_auto(bool auto_rearm);
bool quartz_pay_get_auto(void);


/**
 * Get current payment state.
 */
qz_pay_state_t quartz_pay_get_state(void);

/* v089.12: true after quartz_pay_init() — callers guard on this because
 * re-init resets state to IDLE and would kill an armed watch */
bool quartz_pay_is_initialized(void);

/**
 * Get current payment request info.
 */
const qz_pay_request_t *quartz_pay_get_request(void);

/* ---- v089.12: relay over BLE (docs/RELAY-BLE-SPEC.md) ----
 * One parser, two transports: serial CLI ("relay …") and BLE char 0A0D.
 * Commands (one per call):
 *   ""                      status line
 *   arm <qz> [pulse_s] [fast|safe]   (bare "<qz> …" also accepted)
 *   test [sec] · off|cancel · fast [1|0] · safe · invert [1|0] ·
 *   auto [1|0] · pin <gpio>         (pin persists + reboots)
 * Returns 0 on success, -1 on usage error; one-line reply in `reply`. */
int quartz_pay_relay_cmd(const char *line, char *reply, size_t reply_len);

/* JSON status snapshot for BLE char 0A0C:
 * {"v":1,"state":"idle|armed|receiving|fired|expired|error",...}
 * `uri` present only while armed. Returns length, or -1. */
int quartz_pay_build_relay_json(char *buf, size_t buf_len);

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
