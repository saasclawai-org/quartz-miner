/**
 * Quartz SX1276 LoRa Radio Driver — Register-Level SPI
 *
 * Native C driver for Semtech SX1276/77/78/79 used in:
 *   - LilyGO TTGO LoRa32 V1.6 (SX1276, 868/915MHz)
 *   - Heltec WiFi LoRa 32 V2 (SX1276)
 *   - RFM95/96 breakout boards
 *
 * No external library dependencies. Direct SPI register access.
 *
 * Supported features:
 *   - LoRa mode TX/RX
 *   - Adjustable frequency, bandwidth, spreading factor, coding rate
 *   - CRC generation/checking
 *   - Packet mode with explicit header
 *   - RSSI and SNR measurement
 *   - DIO0 interrupt (TxDone / RxDone)
 *   - CAD (Channel Activity Detection)
 *
 * Pin configuration is set at init time via qz_sx1276_config_t.
 *
 * License: MIT
 */

#ifndef QUARTZ_SX1276_H
#define QUARTZ_SX1276_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================
// SX1276 Register Addresses
// ============================================================

// FIFO operations
#define SX1276_REG_FIFO                 0x00
#define SX1276_REG_OP_MODE              0x01
#define SX1276_REG_FRF_MSB              0x06
#define SX1276_REG_FRF_MID              0x07
#define SX1276_REG_FRF_LSB              0x08
#define SX1276_REG_PA_CONFIG            0x09
#define SX1276_REG_PA_RAMP              0x0A
#define SX1276_REG_OCP                  0x0B
#define SX1276_REG_LNA                  0x0C
#define SX1276_REG_FIFO_ADDR_PTR        0x0D
#define SX1276_REG_FIFO_TX_BASE_ADDR    0x0E
#define SX1276_REG_FIFO_RX_BASE_ADDR    0x0F
#define SX1276_REG_FIFO_RX_CURRENT_ADDR 0x10
#define SX1276_REG_IRQ_FLAGS            0x12
#define SX1276_REG_IRQ_FLAGS_MASK       0x11
#define SX1276_REG_RX_NB_BYTES          0x13
#define SX1276_REG_RX_HEADER_CNT_MSB    0x14
#define SX1276_REG_RX_HEADER_CNT_LSB    0x15
#define SX1276_REG_RX_PACKET_CNT_MSB    0x16
#define SX1276_REG_RX_PACKET_CNT_LSB    0x17
#define SX1276_REG_MODEM_STAT           0x18
#define SX1276_REG_PKT_SNR_VALUE        0x19
#define SX1276_REG_PKT_RSSI_VALUE       0x1A
#define SX1276_REG_RSSI_VALUE           0x1B
#define SX1276_REG_HOP_CHANNEL          0x1C
#define SX1276_REG_MODEM_CONFIG1        0x1D
#define SX1276_REG_MODEM_CONFIG2        0x1E
#define SX1276_REG_SYMB_TIMEOUT_MSB     0x1F
#define SX1276_REG_SYMB_TIMEOUT_LSB     0x20
#define SX1276_REG_PREAMBLE_MSB         0x21
#define SX1276_REG_PREAMBLE_LSB         0x22
#define SX1276_REG_PAYLOAD_LENGTH       0x23
#define SX1276_REG_MAX_PAYLOAD_LENGTH   0x24
#define SX1276_REG_HOP_PERIOD           0x25
#define SX1276_REG_FIFO_RX_BYTE_ADDR    0x26
#define SX1276_REG_MODEM_CONFIG3        0x26
#define SX1276_REG_FEAT                 0x27  // SX1276 feature enable
#define SX1276_REGdetect_optimize       0x31
#define SX1276_REG_INVERTIQ             0x33
#define SX1276_REG_DETECTION_THRESHOLD  0x37
#define SX1276_REG_SYNC_WORD            0x39
#define SX1276_REG_INVERTIQ2            0x41
#define SX1276_REG_DIO_MAPPING1         0x40
#define SX1276_REG_DIO_MAPPING2         0x41
#define SX1276_REG_VERSION              0x42
#define SX1276_REG_PA_DAC               0x4D

// ============================================================
// OP_MODE bits
// ============================================================

#define SX1276_MODE_LORA                0x80
#define SX1276_MODE_FSKOOK              0x00
#define SX1276_MODE_SLEEP               0x00
#define SX1276_MODE_STDBY               0x01
#define SX1276_MODE_FSTX                0x02
#define SX1276_MODE_TX                  0x03
#define SX1276_MODE_FSRX                0x04
#define SX1276_MODE_RXCONTINUOUS        0x05
#define SX1276_MODE_RXSINGLE            0x06
#define SX1276_MODE_CAD                 0x07

// ============================================================
// IRQ flags
// ============================================================

#define SX1276_IRQ_CAD_DETECTED         (1 << 0)
#define SX1276_IRQ_FHSS_CHANGE          (1 << 1)
#define SX1276_IRQ_CAD_DONE             (1 << 2)
#define SX1276_IRQ_TX_DONE              (1 << 3)
#define SX1276_IRQ_VALID_HEADER         (1 << 4)
#define SX1276_IRQ_PAYLOAD_CRC_ERROR    (1 << 5)
#define SX1276_IRQ_RX_DONE              (1 << 6)
#define SX1276_IRQ_RX_TIMEOUT           (1 << 7)

// ============================================================
// PA_CONFIG bits
// ============================================================

#define SX1276_PA_BOOST                 0x80

// ============================================================
// Types
// ============================================================

typedef enum {
    SX1276_BW_7_8     = 0,
    SX1276_BW_10_4    = 1,
    SX1276_BW_15_6    = 2,
    SX1276_BW_20_8    = 3,
    SX1276_BW_31_25   = 4,
    SX1276_BW_41_7    = 5,
    SX1276_BW_62_5    = 6,
    SX1276_BW_125_0   = 7,
    SX1276_BW_250_0   = 8,
    SX1276_BW_500_0   = 9,
} sx1276_bw_t;

typedef enum {
    SX1276_CR_4_5 = 1,
    SX1276_CR_4_6 = 2,
    SX1276_CR_4_7 = 3,
    SX1276_CR_4_8 = 4,
} sx1276_cr_t;

typedef struct {
    // SPI pins
    int spi_host;       // SPI2_HOST (2) or VSPI_HOST (3)
    int pin_sck;
    int pin_miso;
    int pin_mosi;
    int pin_cs;
    int pin_rst;
    int pin_dio0;       // TxDone / RxDone interrupt
    int pin_dio1;       // optional, for CAD (set to -1 if unused)

    // Radio parameters
    uint32_t freq_hz;       // 868100000 for EU, 915000000 for US
    sx1276_bw_t bandwidth;
    uint8_t spreading_factor;  // 6-12
    sx1276_cr_t coding_rate;
    uint8_t sync_word;      // 0x12 = LoRa standard, 0xQU = Quartz private
    int8_t  tx_power_dbm;   // 2-17 (PA_BOOST), 2-20 with PA_DAC
    uint16_t preamble_len;
    bool    crc_on;
    bool    implicit_header;
    uint8_t payload_len;    // only used in implicit header mode
} sx1276_config_t;

typedef struct {
    int16_t rssi;       // dBm
    int8_t  snr;        // dB (can be negative)
    int8_t  snr_frac;   // fractional SNR (0.25 dB units)
    int8_t  packet_rssi;
    bool    crc_error;
    uint8_t hop_channel;
} sx1276_packet_info_t;

// ============================================================
// API
// ============================================================

/**
 * Initialize SX1276 radio.
 * Returns 0 on success, negative on error.
 */
int sx1276_init(const sx1276_config_t *cfg);

/**
 * Shutdown radio (set to sleep mode, release SPI).
 */
void sx1276_deinit(void);

/**
 * Transmit data (blocking). Max 255 bytes.
 * Returns 0 on success, negative on error.
 */
int sx1276_transmit(const uint8_t *data, size_t len);

/**
 * Start receive mode (non-blocking). Use sx1276_check_rx() to poll.
 * Set timeout_ms=0 for single receive mode with symbol timeout,
 * or timeout_ms=SX1276_RX_CONTINUOUS for continuous mode.
 */
int sx1276_start_rx(uint32_t timeout_ms);

/** RX continuous mode flag */
#define SX1276_RX_CONTINUOUS  0xFFFFFFFF

/**
 * Check if a packet was received. Returns packet length if available,
 * 0 if nothing received, negative on error.
 * If info is non-NULL, fills in RSSI/SNR/CRC info.
 */
int sx1276_check_rx(uint8_t *buf, size_t max_len, sx1276_packet_info_t *info);

/**
 * Read last RSSI value (even when not receiving).
 */
int16_t sx1276_get_rssi(void);

/**
 * Set TX power. dBm range depends on PA config.
 * 2-17 with PA_BOOST, up to 20 with PA_DAC override.
 */
int sx1276_set_tx_power(int8_t dbm);

/**
 * Change frequency (Hz). Takes ~1ms.
 */
int sx1276_set_frequency(uint32_t freq_hz);

/**
 * Set spreading factor (6-12). Lower = faster data rate, higher = longer range.
 */
int sx1276_set_spreading_factor(uint8_t sf);

/**
 * Set bandwidth.
 */
int sx1276_set_bandwidth(sx1276_bw_t bw);

/**
 * Enter sleep mode (low power, ~0.2µA).
 */
void sx1276_sleep(void);

/**
 * Enter standby mode.
 */
void sx1276_standby(void);

/**
 * Get chip version (0x12 = SX1276).
 */
uint8_t sx1276_get_version(void);

/**
 * Channel Activity Detection — check if LoRa preamble is present
 * without transmitting. Returns true if channel is active.
 */
bool sx1276_cad_detect(uint32_t timeout_ms);

/**
 * Read a register (for debugging).
 */
uint8_t sx1276_read_reg(uint8_t addr);

/**
 * Write a register (for debugging).
 */
void sx1276_write_reg(uint8_t addr, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif // QUARTZ_SX1276_H
