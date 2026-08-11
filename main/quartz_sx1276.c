/**
 * Quartz SX1276 LoRa Radio Driver — Register-Level SPI Implementation
 *
 * Native ESP-IDF C driver. No Arduino, no RadioLib.
 * Direct SPI register access to Semtech SX1276.
 *
 * Tested on:
 *   - LilyGO TTGO LoRa32 V1.6.1 (ESP32 + SX1276, 868/915MHz)
 *   - RFM95W breakout boards
 *
 * License: MIT
 */

#include "quartz_sx1276.h"
#include "esp_log.h"
#include "esp_system.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

static const char *TAG = "SX1276";

// ============================================================
// State
// ============================================================

static spi_device_handle_t s_spi = NULL;
static sx1276_config_t s_cfg;
static bool s_initialized = false;

// ============================================================
// SPI Helpers
// ============================================================

uint8_t sx1276_read_reg(uint8_t addr) {
    uint8_t tx[2] = { addr & 0x7F, 0x00 };  // bit7=0 for read
    uint8_t rx[2] = { 0 };
    spi_transaction_t t = {
        .length = 16,           // 2 bytes
        .tx_buffer = tx,
        .rx_buffer = rx,
        .flags = SPI_TRANS_USE_RXDATA,
    };
    esp_err_t ret = spi_device_polling_transmit(s_spi, &t);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SPI read failed addr=0x%02X ret=%d", addr, ret);
        return 0;
    }
    return rx[1];
}

void sx1276_write_reg(uint8_t addr, uint8_t val) {
    uint8_t tx[2] = { addr | 0x80, val };  // bit7=1 for write
    spi_transaction_t t = {
        .length = 16,
        .tx_buffer = tx,
        .flags = 0,
    };
    spi_device_polling_transmit(s_spi, &t);
}

// Read multiple registers (burst read)
static void sx1276_read_burst(uint8_t addr, uint8_t *buf, size_t len) {
    // Use a single transaction with the address byte followed by dummy bytes
    uint8_t tx[1] = { addr & 0x7F };
    spi_transaction_t t = {
        .length = 8 + len * 8,
        .tx_buffer = NULL,
        .rx_buffer = NULL,
    };
    // Allocate combined buffer
    uint8_t *txbuf = heap_caps_malloc(1 + len, MALLOC_CAP_DMA);
    uint8_t *rxbuf = heap_caps_malloc(1 + len, MALLOC_CAP_DMA);
    if (!txbuf || !rxbuf) {
        ESP_LOGE(TAG, "Burst read alloc failed");
        free(txbuf); free(rxbuf);
        return;
    }
    txbuf[0] = addr & 0x7F;
    memset(txbuf + 1, 0, len);
    t.tx_buffer = txbuf;
    t.rx_buffer = rxbuf;
    spi_device_polling_transmit(s_spi, &t);
    memcpy(buf, rxbuf + 1, len);
    free(txbuf);
    free(rxbuf);
}

// Write multiple registers (burst write)
static void sx1276_write_burst(uint8_t addr, const uint8_t *buf, size_t len) {
    uint8_t *txbuf = heap_caps_malloc(1 + len, MALLOC_CAP_DMA);
    if (!txbuf) {
        ESP_LOGE(TAG, "Burst write alloc failed");
        return;
    }
    txbuf[0] = addr | 0x80;
    memcpy(txbuf + 1, buf, len);
    spi_transaction_t t = {
        .length = (1 + len) * 8,
        .tx_buffer = txbuf,
    };
    spi_device_polling_transmit(s_spi, &t);
    free(txbuf);
}

// ============================================================
// Mode Control
// ============================================================

static void sx1276_set_mode(uint8_t mode) {
    // Must wake from sleep before changing mode
    uint8_t opmode = sx1276_read_reg(SX1276_REG_OP_MODE);
    // Preserve LoRa/FSK bit
    opmode = (opmode & 0x80) | (mode & 0x07);
    sx1276_write_reg(SX1276_REG_OP_MODE, opmode);
    // Small delay for mode transition
    vTaskDelay(pdMS_TO_TICKS(2));
}

// ============================================================
// Init
// ============================================================

int sx1276_init(const sx1276_config_t *cfg) {
    memcpy(&s_cfg, cfg, sizeof(s_cfg));
    esp_err_t ret;

    // --- GPIO setup ---
    // RST pin
    if (cfg->pin_rst >= 0) {
        gpio_reset_pin(cfg->pin_rst);
        gpio_set_direction(cfg->pin_rst, GPIO_MODE_OUTPUT);
        // Hardware reset: pull low 10ms, release, wait 10ms
        gpio_set_level(cfg->pin_rst, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
        gpio_set_level(cfg->pin_rst, 1);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // --- SPI setup ---
    spi_bus_config_t buscfg = {
        .miso_io_num = cfg->pin_miso,
        .mosi_io_num = cfg->pin_mosi,
        .sclk_io_num = cfg->pin_sck,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 256,
    };

    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = 8 * 1000 * 1000,  // 8 MHz (SX1276 max 10MHz)
        .mode = 0,                           // SPI mode 0 (CPOL=0, CPHA=0)
        .spics_io_num = cfg->pin_cs,
        .queue_size = 4,
        .flags = SPI_DEVICE_HALFDUPLEX,
    };

    ret = spi_bus_initialize(cfg->spi_host, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(ret));
        return -1;
    }

    ret = spi_bus_add_device(cfg->spi_host, &devcfg, &s_spi);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI device add failed: %s", esp_err_to_name(ret));
        return -2;
    }

    // --- Check chip version ---
    uint8_t version = sx1276_read_reg(SX1276_REG_VERSION);
    if (version == 0x00 || version == 0xFF) {
        ESP_LOGE(TAG, "No SX1276 detected (version=0x%02X). Check wiring!", version);
        return -3;
    }
    ESP_LOGI(TAG, "SX1276 version: 0x%02X", version);

    // --- DIO0 pin (interrupt input) ---
    if (cfg->pin_dio0 >= 0) {
        gpio_reset_pin(cfg->pin_dio0);
        gpio_set_direction(cfg->pin_dio0, GPIO_MODE_INPUT);
        gpio_set_pull_mode(cfg->pin_dio0, GPIO_PULLDOWN_ONLY);
    }

    // --- Switch to LoRa mode ---
    // Must be in sleep mode to access config registers in LoRa mode
    sx1276_set_mode(SX1276_MODE_SLEEP);
    vTaskDelay(pdMS_TO_TICKS(5));

    // Verify we're in LoRa mode
    uint8_t opmode = sx1276_read_reg(SX1276_REG_OP_MODE);
    if (!(opmode & SX1276_MODE_LORA)) {
        // Force LoRa mode
        sx1276_write_reg(SX1276_REG_OP_MODE, SX1276_MODE_LORA | SX1276_MODE_SLEEP);
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    // Go to standby while configuring
    sx1276_set_mode(SX1276_MODE_STDBY);

    // --- Set frequency ---
    // FRF = freq_hz * 2^19 / 32MHz
    uint64_t frf = ((uint64_t)cfg->freq_hz << 19) / 32000000ULL;
    sx1276_write_reg(SX1276_REG_FRF_MSB, (frf >> 16) & 0xFF);
    sx1276_write_reg(SX1276_REG_FRF_MID, (frf >> 8) & 0xFF);
    sx1276_write_reg(SX1276_REG_FRF_LSB, frf & 0xFF);

    // --- PA config ---
    if (cfg->tx_power_dbm > 17) {
        // Use PA_DAC for 18-20 dBm
        sx1276_write_reg(SX1276_REG_PA_DAC, 0x87);  // 20 dBm
        sx1276_write_reg(SX1276_REG_PA_CONFIG,
            SX1276_PA_BOOST | (cfg->tx_power_dbm - 2));  // PA_BOOST, max power
    } else {
        sx1276_write_reg(SX1276_REG_PA_DAC, 0x84);  // normal
        // PA_BOOST pin, output power = 17 - (15 - tx_power) = tx_power + 2
        // Actually: Pout = 2 + (outpower & 0x0F), and we set PA_BOOST bit
        uint8_t pwr = cfg->tx_power_dbm - 2;
        if (pwr > 15) pwr = 15;
        sx1276_write_reg(SX1276_REG_PA_CONFIG, SX1276_PA_BOOST | pwr);
    }

    // --- OCP (Over Current Protection) ---
    sx1276_write_reg(SX1276_REG_OCP, 0x3F);  // ~240mA

    // --- LNA ---
    sx1276_write_reg(SX1276_REG_LNA, 0x23);  // G1 max gain, LNA boost

    // --- Modem config 1 ---
    // BW[4:0] | CR[2:0] | implicit_header | rx_payload_crc
    // Actually for SX1276:
    // Config1: BW[3:0] (bits 7-4), CR[2:0] (bits 3-1), ImplicitHeaderMode (bit 0)
    uint8_t config1 = ((cfg->bandwidth & 0x0F) << 4) |
                      ((cfg->coding_rate & 0x07) << 1) |
                      (cfg->implicit_header ? 1 : 0);
    sx1276_write_reg(SX1276_REG_MODEM_CONFIG1, config1);

    // --- Modem config 2 ---
    // SF[3:0] (bits 7-4) | TxContMode (bit 3) | RxPayloadCrcOn (bit 2) | SymbTimeoutMSB[1:0] (bits 1-0)
    uint8_t config2 = ((cfg->spreading_factor & 0x0F) << 4) |
                      (cfg->crc_on ? 0x04 : 0x00);
    sx1276_write_reg(SX1276_REG_MODEM_CONFIG2, config2);

    // --- Modem config 3 ---
    // LowDataRateOptimize (bit 3) | AGCAutoOn (bit 2)
    uint8_t low_data_rate = 0;
    // Enable low data rate optimize for SF11/12 or long symbols
    if (cfg->spreading_factor >= 11) {
        low_data_rate = 0x08;  // MobileNode/LowDataRateOptimize
    }
    sx1276_write_reg(0x26, low_data_rate | 0x04);  // AGC auto on

    // --- Sync word ---
    sx1276_write_reg(SX1276_REG_SYNC_WORD, cfg->sync_word);

    // --- Preamble length ---
    sx1276_write_reg(SX1276_REG_PREAMBLE_MSB, (cfg->preamble_len >> 8) & 0xFF);
    sx1276_write_reg(SX1276_REG_PREAMBLE_LSB, cfg->preamble_len & 0xFF);

    // --- Max payload length ---
    sx1276_write_reg(SX1276_REG_MAX_PAYLOAD_LENGTH, 255);

    // --- FIFO base addresses ---
    sx1276_write_reg(SX1276_REG_FIFO_TX_BASE_ADDR, 0x00);
    sx1276_write_reg(SX1276_REG_FIFO_RX_BASE_ADDR, 0x00);

    // --- Detection optimize (for SF6, need special handling) ---
    if (cfg->spreading_factor == 6) {
        sx1276_write_reg(0x31, 0xC5);  // DetectionOptimize for SF6
        sx1276_write_reg(SX1276_REG_DETECTION_THRESHOLD, 0x0C);
    } else {
        sx1276_write_reg(0x31, 0xC3);  // DetectionOptimize for SF7-12
        sx1276_write_reg(SX1276_REG_DETECTION_THRESHOLD, 0x0A);
    }

    // --- DIO mapping: DIO0 = TxDone(0)/RxDone(0) ---
    // In LoRa mode, DIO0 is mapped by bits 7-6 of DIO_MAPPING1
    // TX: 00 → TxDone, RX: 00 → RxDone
    sx1276_write_reg(SX1276_REG_DIO_MAPPING1, 0x00);

    // --- IRQ mask: enable TxDone, RxDone, RxTimeout ---
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS_MASK, 0x00);  // all enabled

    // Clear any pending IRQs
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS, 0xFF);

    // --- Go to standby ---
    sx1276_set_mode(SX1276_MODE_STDBY);

    s_initialized = true;

    ESP_LOGI(TAG, "SX1276 initialized: %lu Hz, SF%d, BW=%d, CR=4/%d, TX=%d dBm, sync=0x%02X",
             (unsigned long)cfg->freq_hz, cfg->spreading_factor,
             cfg->bandwidth, cfg->coding_rate + 4, cfg->tx_power_dbm, cfg->sync_word);

    return 0;
}

// ============================================================
// Deinit
// ============================================================

void sx1276_deinit(void) {
    if (!s_initialized) return;
    sx1276_sleep();
    spi_bus_remove_device(s_spi);
    s_spi = NULL;
    s_initialized = false;
}

// ============================================================
// Transmit
// ============================================================

int sx1276_transmit(const uint8_t *data, size_t len) {
    if (!s_initialized) return -1;
    if (len > 255) return -2;

    // Set to standby
    sx1276_set_mode(SX1276_MODE_STDBY);

    // Set DIO0 to TxDone
    sx1276_write_reg(SX1276_REG_DIO_MAPPING1, 0x40);  // 01xxxxxx = TxDone on DIO0

    // Clear TX done IRQ
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_TX_DONE);

    // Set FIFO address
    sx1276_write_reg(SX1276_REG_FIFO_ADDR_PTR, 0x00);
    sx1276_write_reg(SX1276_REG_FIFO_TX_BASE_ADDR, 0x00);

    // Write payload to FIFO
    sx1276_write_burst(SX1276_REG_FIFO, data, len);

    // Set payload length
    sx1276_write_reg(SX1276_REG_PAYLOAD_LENGTH, len);

    // Start TX
    sx1276_set_mode(SX1276_MODE_TX);

    // Wait for TxDone (poll DIO0 or IRQ flag)
    // Typical TX time at SF7/125kHz: ~50ms for 64 bytes
    uint32_t timeout_ms = 3000;  // 3 second timeout
    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;

    while (1) {
        // Check IRQ flag
        uint8_t irq = sx1276_read_reg(SX1276_REG_IRQ_FLAGS);
        if (irq & SX1276_IRQ_TX_DONE) {
            sx1276_write_reg(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_TX_DONE);
            break;
        }
        // Check DIO0 pin
        if (s_cfg.pin_dio0 >= 0 && gpio_get_level(s_cfg.pin_dio0)) {
            break;
        }
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;
        if (elapsed > timeout_ms) {
            ESP_LOGW(TAG, "TX timeout after %dms", elapsed);
            sx1276_set_mode(SX1276_MODE_STDBY);
            return -3;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    // Back to standby
    sx1276_set_mode(SX1276_MODE_STDBY);

    return 0;
}

// ============================================================
// Receive
// ============================================================

int sx1276_start_rx(uint32_t timeout_ms) {
    if (!s_initialized) return -1;

    sx1276_set_mode(SX1276_MODE_STDBY);

    // Set DIO0 to RxDone
    sx1276_write_reg(SX1276_REG_DIO_MAPPING1, 0x00);  // 00xxxxxx = RxDone on DIO0

    // Clear IRQ flags
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS, 0xFF);

    // Reset FIFO address
    sx1276_write_reg(SX1276_REG_FIFO_ADDR_PTR, 0x00);
    sx1276_write_reg(SX1276_REG_FIFO_RX_BASE_ADDR, 0x00);

    if (timeout_ms == SX1276_RX_CONTINUOUS) {
        sx1276_set_mode(SX1276_MODE_RXCONTINUOUS);
    } else {
        // Set symbol timeout (rough mapping ms → symbols)
        // Each symbol = 2^SF / BW seconds
        // At SF7/125kHz: 1 symbol = 1.024ms
        // timeout_ms / 1.024 ≈ symbols
        uint16_t sym_timeout = timeout_ms / 2;  // conservative
        if (sym_timeout < 4) sym_timeout = 4;
        if (sym_timeout > 1023) sym_timeout = 1023;
        sx1276_write_reg(SX1276_REG_SYMB_TIMEOUT_MSB, (sym_timeout >> 8) & 0x03);
        sx1276_write_reg(SX1276_REG_SYMB_TIMEOUT_LSB, sym_timeout & 0xFF);

        sx1276_set_mode(SX1276_MODE_RXSINGLE);
    }

    return 0;
}

int sx1276_check_rx(uint8_t *buf, size_t max_len, sx1276_packet_info_t *info) {
    if (!s_initialized) return -1;

    // Check IRQ flags
    uint8_t irq = sx1276_read_reg(SX1276_REG_IRQ_FLAGS);

    if (irq & SX1276_IRQ_RX_TIMEOUT) {
        sx1276_write_reg(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_RX_TIMEOUT);
        return 0;  // nothing received
    }

    if (irq & SX1276_IRQ_PAYLOAD_CRC_ERROR) {
        sx1276_write_reg(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_PAYLOAD_CRC_ERROR);
        if (info) {
            info->crc_error = true;
            info->packet_rssi = sx1276_read_reg(SX1276_REG_PKT_RSSI_VALUE) - 157;
        }
        ESP_LOGD(TAG, "CRC error on received packet");
        return -2;
    }

    if (!(irq & SX1276_IRQ_RX_DONE)) {
        return 0;  // nothing yet
    }

    // RxDone — clear IRQ
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS, SX1276_IRQ_RX_DONE);

    // Read packet info
    int8_t snr_raw = (int8_t)sx1276_read_reg(SX1276_REG_PKT_SNR_VALUE);
    uint8_t rssi_raw = sx1276_read_reg(SX1276_REG_PKT_RSSI_VALUE);

    // RSSI calculation: RSSI = rssi_raw - 157 (HF port > 860MHz)
    // For LF port < 860MHz: RSSI = rssi_raw - 164
    int16_t rssi;
    if (s_cfg.freq_hz > 860000000) {
        rssi = rssi_raw - 157;
    } else {
        rssi = rssi_raw - 164;
    }

    // SNR: value is 2's complement in 0.25 dB units
    float snr_db;
    if (snr_raw < 0) {
        snr_db = snr_raw / 4.0;
    } else {
        snr_db = snr_raw / 4.0;
    }

    if (info) {
        info->rssi = rssi;
        info->packet_rssi = rssi;
        info->snr = (int8_t)snr_db;
        info->snr_frac = (snr_raw & 0x03);
        info->crc_error = false;
        info->hop_channel = sx1276_read_reg(SX1276_REG_HOP_CHANNEL);
    }

    // Read FIFO
    uint8_t rx_addr = sx1276_read_reg(SX1276_REG_FIFO_RX_CURRENT_ADDR);
    sx1276_write_reg(SX1276_REG_FIFO_ADDR_PTR, rx_addr);

    uint8_t nb_bytes = sx1276_read_reg(SX1276_REG_RX_NB_BYTES);
    if (nb_bytes > max_len) nb_bytes = max_len;

    sx1276_read_burst(SX1276_REG_FIFO, buf, nb_bytes);

    ESP_LOGD(TAG, "RX %d bytes, RSSI=%d dBm, SNR=%.1f dB", nb_bytes, rssi, snr_db);

    return nb_bytes;
}

// ============================================================
// Status / Configuration
// ============================================================

int16_t sx1276_get_rssi(void) {
    uint8_t raw = sx1276_read_reg(SX1276_REG_RSSI_VALUE);
    if (s_cfg.freq_hz > 860000000) {
        return raw - 157;
    } else {
        return raw - 164;
    }
}

int sx1276_set_tx_power(int8_t dbm) {
    if (!s_initialized) return -1;
    sx1276_standby();

    if (dbm > 17) {
        sx1276_write_reg(SX1276_REG_PA_DAC, 0x87);
        sx1276_write_reg(SX1276_REG_PA_CONFIG, SX1276_PA_BOOST | (dbm - 2));
    } else {
        sx1276_write_reg(SX1276_REG_PA_DAC, 0x84);
        uint8_t pwr = dbm - 2;
        if (pwr > 15) pwr = 15;
        sx1276_write_reg(SX1276_REG_PA_CONFIG, SX1276_PA_BOOST | pwr);
    }
    s_cfg.tx_power_dbm = dbm;
    return 0;
}

int sx1276_set_frequency(uint32_t freq_hz) {
    if (!s_initialized) return -1;
    sx1276_standby();

    uint64_t frf = ((uint64_t)freq_hz << 19) / 32000000ULL;
    sx1276_write_reg(SX1276_REG_FRF_MSB, (frf >> 16) & 0xFF);
    sx1276_write_reg(SX1276_REG_FRF_MID, (frf >> 8) & 0xFF);
    sx1276_write_reg(SX1276_REG_FRF_LSB, frf & 0xFF);
    s_cfg.freq_hz = freq_hz;
    return 0;
}

int sx1276_set_spreading_factor(uint8_t sf) {
    if (!s_initialized || sf < 6 || sf > 12) return -1;
    sx1276_standby();

    uint8_t config2 = sx1276_read_reg(SX1276_REG_MODEM_CONFIG2);
    config2 = (config2 & 0x0F) | ((sf & 0x0F) << 4);
    sx1276_write_reg(SX1276_REG_MODEM_CONFIG2, config2);

    // Detection optimize
    if (sf == 6) {
        sx1276_write_reg(0x31, 0xC5);
        sx1276_write_reg(SX1276_REG_DETECTION_THRESHOLD, 0x0C);
    } else {
        sx1276_write_reg(0x31, 0xC3);
        sx1276_write_reg(SX1276_REG_DETECTION_THRESHOLD, 0x0A);
    }

    // Low data rate optimize
    if (sf >= 11) {
        uint8_t cfg3 = sx1276_read_reg(0x26);
        sx1276_write_reg(0x26, cfg3 | 0x08);
    } else {
        uint8_t cfg3 = sx1276_read_reg(0x26);
        sx1276_write_reg(0x26, cfg3 & ~0x08);
    }

    s_cfg.spreading_factor = sf;
    return 0;
}

int sx1276_set_bandwidth(sx1276_bw_t bw) {
    if (!s_initialized) return -1;
    sx1276_standby();

    uint8_t config1 = sx1276_read_reg(SX1276_REG_MODEM_CONFIG1);
    config1 = (config1 & 0x0F) | ((bw & 0x0F) << 4);
    sx1276_write_reg(SX1276_REG_MODEM_CONFIG1, config1);
    s_cfg.bandwidth = bw;
    return 0;
}

void sx1276_sleep(void) {
    if (!s_initialized) return;
    sx1276_set_mode(SX1276_MODE_SLEEP);
}

void sx1276_standby(void) {
    if (!s_initialized) return;
    sx1276_set_mode(SX1276_MODE_STDBY);
}

uint8_t sx1276_get_version(void) {
    return sx1276_read_reg(SX1276_REG_VERSION);
}

// ============================================================
// CAD (Channel Activity Detection)
// ============================================================

bool sx1276_cad_detect(uint32_t timeout_ms) {
    if (!s_initialized) return false;

    sx1276_standby();
    sx1276_write_reg(SX1276_REG_DIO_MAPPING1, 0x80);  // CAD on DIO0/DIO1
    sx1276_write_reg(SX1276_REG_IRQ_FLAGS, 0xFF);

    sx1276_set_mode(SX1276_MODE_CAD);

    uint32_t start = xTaskGetTickCount() * portTICK_PERIOD_MS;
    while (1) {
        uint8_t irq = sx1276_read_reg(SX1276_REG_IRQ_FLAGS);
        if (irq & SX1276_IRQ_CAD_DETECTED) {
            sx1276_write_reg(SX1276_REG_IRQ_FLAGS, 0xFF);
            return true;  // activity detected
        }
        if (irq & SX1276_IRQ_CAD_DONE) {
            sx1276_write_reg(SX1276_REG_IRQ_FLAGS, 0xFF);
            return false;  // channel is clear
        }
        uint32_t elapsed = (xTaskGetTickCount() * portTICK_PERIOD_MS) - start;
        if (elapsed > timeout_ms) return false;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// Public accessors for debugging
// (sx1276_read_reg and sx1276_write_reg are already public in the header)
