/**
 * quartz_ble.c — Bluedroid GATT server for Quartz ESP32
 * Exposes mining stats and wallet address over BLE
 */

#include "quartz_ble.h"
#include "quartz_wifi.h"
#include "quartz_pay.h"     /* v089.12: relay over BLE (0A0C/0A0D) */
#include "esp_timer.h"
#include "quartz_wallet.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "QZ.BLE";

/* Packed mining stats for BLE transfer */
struct __attribute__((packed)) mining_stats {
    uint32_t hash_count;
    uint32_t hash_rate;
    uint32_t blocks_found;
    uint32_t uptime;
};

static struct mining_stats s_stats = {0};
static char s_address[64] = {0};

/* Seed phrase provisioning state */
static char s_seed_phrase[12][12] = {0};  /* 12 words, up to 11 chars each */
static bool s_seed_available = false;
static bool s_seed_confirmed = false;
static uint16_t s_seed_handle = 0;
static uint16_t s_confirm_handle = 0;
static uint16_t s_pin_set_handle = 0;
static uint16_t s_pin_unlock_handle = 0;
static uint16_t s_pin_status_handle = 0;
static uint16_t s_wifi_ssid_handle = 0;
static uint16_t s_wifi_pass_handle = 0;
static uint16_t s_reboot_handle = 0;
/* v089.12: relay over BLE — chars appended AFTER 0A0B, never inserted
 * (the v089.6 +1 handle bug: mid-table additions shift every handle) */
static uint16_t s_relay_status_handle = 0;
static uint16_t s_relay_cmd_handle = 0;
static char s_relay_json[256] = "{\"v\":1}";
static bool s_connected = false;
static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_stats_handle = 0;
static uint16_t s_addr_handle = 0;
static esp_gatt_if_t s_gatts_if = 0;

/* PIN unlock state — gates seed/signing access */
static bool s_device_unlocked = true;  /* unlocked by default if no PIN set */
static uint8_t s_pin_status_buf[3] = {0};  /* has_pin, attempts_left, unlocked */

/* GATT service UUID: 00000A01-0000-1000-8000-00805F9B34FB */
static uint8_t s_service_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x01, 0x0A, 0x00, 0x00
};

/* Stats char UUID: 00000A02-... */
static uint8_t s_stats_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x02, 0x0A, 0x00, 0x00
};

/* Address char UUID: 00000A03-... */
static uint8_t s_addr_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x03, 0x0A, 0x00, 0x00
};

/* Seed phrase char UUID: 00000A04-... */
static uint8_t s_seed_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x04, 0x0A, 0x00, 0x00
};

/* Confirm char UUID: 00000A05-... */
static uint8_t s_confirm_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x05, 0x0A, 0x00, 0x00
};

/* PIN set char UUID: 00000A06-... */
static uint8_t s_pin_set_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x06, 0x0A, 0x00, 0x00
};

/* PIN unlock char UUID: 00000A07-... */
static uint8_t s_pin_unlock_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x07, 0x0A, 0x00, 0x00
};

/* PIN status char UUID: 00000A08-... */
static uint8_t s_pin_status_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x08, 0x0A, 0x00, 0x00
};

/* WiFi SSID char UUID: 00000A09-... (v089.9: BLE provisioning loop) */
static uint8_t s_wifi_ssid_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x09, 0x0A, 0x00, 0x00
};

/* WiFi password char UUID: 00000A0A-... */
static uint8_t s_wifi_pass_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x0A, 0x0A, 0x00, 0x00
};

/* Reboot char UUID: 00000A0B-... (write anything → apply + restart) */
static uint8_t s_reboot_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x0B, 0x0A, 0x00, 0x00
};

/* Relay status char UUID: 00000A0C-... (v089.12: JSON snapshot, read) */
static uint8_t s_relay_status_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x0C, 0x0A, 0x00, 0x00
};

/* Relay command char UUID: 00000A0D-... (v089.12: bonded write, CLI syntax) */
static uint8_t s_relay_cmd_uuid128[16] = {
    0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80,
    0x00, 0x10, 0x00, 0x00, 0x0D, 0x0A, 0x00, 0x00
};

/* v086: BLE payloads must fit 31 bytes. The old scan-rsp (name 14 +
 * UUID 18 + conn-int 6 = 38 B) NEVER fit — config_adv_data failed, the
 * completion event never fired, and advertising never started on ANY
 * build. ADV = flags + UUID (21 B); scan-rsp = name only (14 B). */
static esp_ble_adv_data_t s_adv_data_adv = {
    .set_scan_rsp = false,
    .include_name = false,
    .include_txpower = false,
    .min_interval = 0,
    .max_interval = 0,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = s_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
};

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0,
    .max_interval = 0,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = 0,
};

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x40,
    .adv_int_max = 0x80,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

enum {
    QUARTZ_IDX_SVC,
    QUARTZ_IDX_STATS_CHAR,
    QUARTZ_IDX_STATS_VAL,
    QUARTZ_IDX_STATS_CCCD,     /* v089.2: stats subscription */
    QUARTZ_IDX_ADDR_CHAR,
    QUARTZ_IDX_ADDR_VAL,
    QUARTZ_IDX_SEED_CHAR,
    QUARTZ_IDX_SEED_VAL,
    QUARTZ_IDX_CONFIRM_CHAR,
    QUARTZ_IDX_CONFIRM_VAL,
    QUARTZ_IDX_PIN_SET_CHAR,
    QUARTZ_IDX_PIN_SET_VAL,
    QUARTZ_IDX_PIN_UNLOCK_CHAR,
    QUARTZ_IDX_PIN_UNLOCK_VAL,
    QUARTZ_IDX_PIN_STATUS_CHAR,
    QUARTZ_IDX_PIN_STATUS_VAL,
    QUARTZ_IDX_WIFI_SSID_CHAR,
    QUARTZ_IDX_WIFI_SSID_VAL,
    QUARTZ_IDX_WIFI_PASS_CHAR,
    QUARTZ_IDX_WIFI_PASS_VAL,
    QUARTZ_IDX_REBOOT_CHAR,
    QUARTZ_IDX_REBOOT_VAL,
    /* v089.12: relay over BLE — APPEND-ONLY entries (handle-shift lesson) */
    QUARTZ_IDX_RELAY_STATUS_CHAR,
    QUARTZ_IDX_RELAY_STATUS_VAL,
    QUARTZ_IDX_RELAY_CMD_CHAR,
    QUARTZ_IDX_RELAY_CMD_VAL,
    QUARTZ_IDX_NB,
};

/* v089.2-parity: correct GATT attribute types (mirrors the C3 fix that
 * made the service discoverable). The old table used the service UUID as
 * the entry TYPE and 0x2901 + 2-byte values for char declarations — the
 * stack accepted it, but no client could ever discover the service
 * ("Quartz service not found"). */
static const uint16_t s_pri_service_uuid16 = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t s_char_decl_uuid16  = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t s_cccd_uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
static uint16_t s_stats_ccc = 0;   /* mutable — the stack writes it */

static const uint8_t s_props_stats      = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static const uint8_t s_props_read       = ESP_GATT_CHAR_PROP_BIT_READ;
static const uint8_t s_props_read_write = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE;
static const uint8_t s_props_write      = ESP_GATT_CHAR_PROP_BIT_WRITE;

static esp_gatts_attr_db_t s_attr_db[QUARTZ_IDX_NB] = {
    /* Service */
    [QUARTZ_IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_pri_service_uuid16, ESP_GATT_PERM_READ,
         sizeof(s_service_uuid128), sizeof(s_service_uuid128), s_service_uuid128}
    },
    /* Stats characteristic declaration */
    [QUARTZ_IDX_STATS_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_stats}
    },
    /* Stats characteristic value */
    [QUARTZ_IDX_STATS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_stats_uuid128, ESP_GATT_PERM_READ,
         sizeof(struct mining_stats), sizeof(struct mining_stats), (uint8_t*)&s_stats}
    },
    /* v089.2: Client Characteristic Configuration — subscribe for stats */
    [QUARTZ_IDX_STATS_CCCD] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_cccd_uuid16, ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&s_stats_ccc}
    },
    /* Address characteristic declaration */
    [QUARTZ_IDX_ADDR_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_read}
    },
    /* Address characteristic value */
    [QUARTZ_IDX_ADDR_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_addr_uuid128, ESP_GATT_PERM_READ,
         sizeof(s_address), sizeof(s_address), (uint8_t*)s_address}
    },
    /* Seed phrase characteristic declaration */
    [QUARTZ_IDX_SEED_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_read}
    },
    /* Seed phrase value — requires bonded connection (encrypted)
     * Uses ESP_GATT_PERM_READ_ENCRYPTED so only paired devices can read */
    [QUARTZ_IDX_SEED_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_seed_uuid128,
         ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
         sizeof(s_seed_phrase), sizeof(s_seed_phrase), (uint8_t*)s_seed_phrase}
    },
    /* Confirm characteristic declaration */
    [QUARTZ_IDX_CONFIRM_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_read_write}
    },
    /* Confirm value — phone writes to confirm seed (requires bonding) */
    [QUARTZ_IDX_CONFIRM_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_confirm_uuid128,
         ESP_GATT_PERM_READ_ENCRYPTED | ESP_GATT_PERM_WRITE_ENCRYPTED,
         16, 0, NULL}
    },
    /* PIN set characteristic declaration */
    [QUARTZ_IDX_PIN_SET_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    /* PIN set value — write PIN string (requires bonding) */
    [QUARTZ_IDX_PIN_SET_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_pin_set_uuid128,
         ESP_GATT_PERM_WRITE_ENCRYPTED,
         10, 0, NULL}
    },
    /* PIN unlock characteristic declaration */
    [QUARTZ_IDX_PIN_UNLOCK_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    /* PIN unlock value — write PIN to unlock device */
    [QUARTZ_IDX_PIN_UNLOCK_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_pin_unlock_uuid128,
         ESP_GATT_PERM_WRITE_ENCRYPTED,
         10, 0, NULL}
    },
    /* PIN status characteristic declaration */
    [QUARTZ_IDX_PIN_STATUS_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_read}
    },
    /* PIN status value — read: [has_pin, attempts_left, unlocked] */
    [QUARTZ_IDX_PIN_STATUS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_pin_status_uuid128, ESP_GATT_PERM_READ,
         sizeof(s_pin_status_buf), sizeof(s_pin_status_buf), s_pin_status_buf}
    },
    /* v089.9: WiFi provisioning over BLE — write SSID, then password,
     * then reboot. Encrypted (bonded) writes only. */
    [QUARTZ_IDX_WIFI_SSID_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    [QUARTZ_IDX_WIFI_SSID_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_wifi_ssid_uuid128, ESP_GATT_PERM_WRITE_ENCRYPTED,
         33, 0, NULL}
    },
    [QUARTZ_IDX_WIFI_PASS_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    [QUARTZ_IDX_WIFI_PASS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_wifi_pass_uuid128, ESP_GATT_PERM_WRITE_ENCRYPTED,
         65, 0, NULL}
    },
    [QUARTZ_IDX_REBOOT_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    [QUARTZ_IDX_REBOOT_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_reboot_uuid128, ESP_GATT_PERM_WRITE_ENCRYPTED,
         1, 0, NULL}
    },
    /* v089.12: relay status — JSON snapshot, refreshed on read */
    [QUARTZ_IDX_RELAY_STATUS_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_read}
    },
    [QUARTZ_IDX_RELAY_STATUS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_relay_status_uuid128, ESP_GATT_PERM_READ,
         sizeof(s_relay_json), sizeof(s_relay_json), (uint8_t*)s_relay_json}
    },
    /* v089.12: relay command — bonded write, CLI syntax (RELAY-BLE-SPEC.md) */
    [QUARTZ_IDX_RELAY_CMD_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&s_char_decl_uuid16, ESP_GATT_PERM_READ,
         sizeof(uint8_t), sizeof(uint8_t), (uint8_t*)&s_props_write}
    },
    [QUARTZ_IDX_RELAY_CMD_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_relay_cmd_uuid128, ESP_GATT_PERM_WRITE_ENCRYPTED,
         96, 0, NULL}
    },
};

static bool s_advertising = false;         /* v087: ADV_START_COMPLETE seen */
static bool s_adv_allowed = false;        /* v089.8: wallet ready → allowed to advertise.
                                          * Fresh boots bring the BLE stack up ~80s before
                                          * the wallet exists (entropy gate); pairing in that
                                          * window read zero words and the app dead-ended
                                          * on a false "already backed up". The radio stays
                                          * on for RNG — it just doesn't advertise yet. */

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        /* v086: advertise as soon as the ADV payload is set; the scan-rsp
         * (name only) applies live even if it completes after start. */
        if (s_adv_allowed) esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_SET_COMPLETE_EVT:
        break;  /* nothing to do — name rides in the scan response */
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_advertising = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        if (!s_advertising) {
            ESP_LOGE(TAG, "Advertising start failed (status %d)",
                     param->adv_start_cmpl.status);
        } else {
            ESP_LOGI(TAG, "BLE advertising as \"Quartz-Miner\"");
        }
        break;
    case ESP_GAP_BLE_SEC_REQ_EVT:
        /* Security request from phone — accept */
        esp_ble_gap_security_rsp(param->ble_security.ble_req.bd_addr, true);
        ESP_LOGI(TAG, "BLE security request from %02x:%02x:%02x:%02x:%02x:%02x",
                 param->ble_security.ble_req.bd_addr[0], param->ble_security.ble_req.bd_addr[1],
                 param->ble_security.ble_req.bd_addr[2], param->ble_security.ble_req.bd_addr[3],
                 param->ble_security.ble_req.bd_addr[4], param->ble_security.ble_req.bd_addr[5]);
        break;
    case ESP_GAP_BLE_AUTH_CMPL_EVT:
        if (param->ble_security.auth_cmpl.success) {
            ESP_LOGI(TAG, "✅ BLE bonded with %02x:%02x:%02x:%02x:%02x:%02x",
                     param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                     param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                     param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
        } else {
            ESP_LOGW(TAG, "❌ BLE bond failed with %02x:%02x:%02x:%02x:%02x:%02x",
                     param->ble_security.auth_cmpl.bd_addr[0], param->ble_security.auth_cmpl.bd_addr[1],
                     param->ble_security.auth_cmpl.bd_addr[2], param->ble_security.auth_cmpl.bd_addr[3],
                     param->ble_security.auth_cmpl.bd_addr[4], param->ble_security.auth_cmpl.bd_addr[5]);
        }
        break;
    default:
        break;
    }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) {
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "GATTS register failed: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        esp_ble_gatts_create_attr_tab(s_attr_db, gatts_if, QUARTZ_IDX_NB, 0);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "Create attr db failed: %d", param->add_attr_tab.status);
            return;
        }
        if (param->add_attr_tab.num_handle == QUARTZ_IDX_NB) {
            s_stats_handle = param->add_attr_tab.handles[QUARTZ_IDX_STATS_VAL];
            s_addr_handle = param->add_attr_tab.handles[QUARTZ_IDX_ADDR_VAL];
            s_seed_handle = param->add_attr_tab.handles[QUARTZ_IDX_SEED_VAL];
            s_confirm_handle = param->add_attr_tab.handles[QUARTZ_IDX_CONFIRM_VAL];
            s_pin_set_handle = param->add_attr_tab.handles[QUARTZ_IDX_PIN_SET_VAL];
            s_pin_unlock_handle = param->add_attr_tab.handles[QUARTZ_IDX_PIN_UNLOCK_VAL];
            s_pin_status_handle = param->add_attr_tab.handles[QUARTZ_IDX_PIN_STATUS_VAL];
            s_wifi_ssid_handle = param->add_attr_tab.handles[QUARTZ_IDX_WIFI_SSID_VAL];
            s_wifi_pass_handle = param->add_attr_tab.handles[QUARTZ_IDX_WIFI_PASS_VAL];
            s_reboot_handle = param->add_attr_tab.handles[QUARTZ_IDX_REBOOT_VAL];
            s_relay_status_handle = param->add_attr_tab.handles[QUARTZ_IDX_RELAY_STATUS_VAL];
            s_relay_cmd_handle = param->add_attr_tab.handles[QUARTZ_IDX_RELAY_CMD_VAL];
            if (esp_ble_gatts_start_service(param->add_attr_tab.handles[QUARTZ_IDX_SVC]) != ESP_OK)
                ESP_LOGE(TAG, "GATT service start failed");
            /* v086: both payloads sized to fit 31 bytes — if these calls
             * fail, we now log it loudly instead of failing silently.
             * v089.8: only once the wallet is ready to be served. */
            if (s_adv_allowed) {
                if (esp_ble_gap_config_adv_data(&s_adv_data_adv) != ESP_OK)
                    ESP_LOGE(TAG, "ADV payload rejected (too big?) — check sizes");
                if (esp_ble_gap_config_adv_data(&s_adv_data) != ESP_OK)
                    ESP_LOGE(TAG, "scan-rsp payload rejected (too big?) — check sizes");
            }
        } else {
            ESP_LOGE(TAG, "Attr table count mismatch: got %d, want %d",
                     param->add_attr_tab.num_handle, QUARTZ_IDX_NB);
        }
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_connected = true;
        s_conn_handle = param->connect.conn_id;
        ESP_LOGI(TAG, "Phone connected to BLE");
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_conn_handle = 0xFFFF;
        ESP_LOGI(TAG, "Phone disconnected from BLE");
        if (s_adv_allowed) esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_stats_handle) {
            esp_ble_gatts_set_attr_value(s_stats_handle,
                sizeof(struct mining_stats), (uint8_t*)&s_stats);
        }
        /* v0893: attr values are COPIED at table creation — without these
         * refreshes the phone keeps reading the boot-time zeros ("0 words",
         * empty address) no matter what the setters wrote into RAM. */
        if (param->read.handle == s_addr_handle) {
            esp_ble_gatts_set_attr_value(s_addr_handle,
                strlen(s_address), (uint8_t*)s_address);
        }
        if (param->read.handle == s_seed_handle) {
            if (s_seed_confirmed) {
                /* v089.7: serve the zeroed buffer, NOT a 0-length value —
                 * Bluedroid's SET_ATTR_VALUE deep-copy rejects len 0
                 * ("btc_gatts_arg_deep_copy 12, invalid length" spam), the
                 * set fails, the table keeps serving the words, and the
                 * app re-shows the confirmation card forever. The buffer
                 * was memset to 0 at confirm (and boots zeroed for
                 * already-confirmed boards). */
                esp_ble_gatts_set_attr_value(s_seed_handle,
                    sizeof(s_seed_phrase), (uint8_t*)s_seed_phrase);
            } else if (s_seed_available) {
                /* Provisioning: serve the words — PERM_READ_ENCRYPTED
                 * already gates access to bonded peers */
                esp_ble_gatts_set_attr_value(s_seed_handle,
                    sizeof(s_seed_phrase), (uint8_t*)s_seed_phrase);
            }
        }
        /* v089.12: relay status JSON snapshot (0A0C) */
        if (param->read.handle == s_relay_status_handle) {
            quartz_pay_build_relay_json(s_relay_json, sizeof(s_relay_json));
            esp_ble_gatts_set_attr_value(s_relay_status_handle,
                (uint16_t) strlen(s_relay_json), (uint8_t*) s_relay_json);
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_confirm_handle) {
            /* Phone sent 3 word indices (0-11) as confirmation */
            if (param->write.len >= 3 && s_seed_available && !s_seed_confirmed) {
                uint8_t *indices = param->write.value;
                /* TODO: verify the 3 indices match the expected words */
                /* For now, any 3-byte write confirms */
                s_seed_confirmed = true;
                s_seed_available = false;
                /* Zero out seed phrase from RAM */
                memset(s_seed_phrase, 0, sizeof(s_seed_phrase));
                ESP_LOGI(TAG, "Seed phrase confirmed via BLE — wiped from RAM");
            }
        }
        /* PIN set — requires bonded encrypted connection */
        if (param->write.handle == s_pin_set_handle) {
            if (param->write.len > 0 && param->write.len <= 8) {
                char pin[9] = {0};
                memcpy(pin, param->write.value, param->write.len);
                pin[param->write.len] = '\0';
                quartz_wallet_set_pin(pin);
                memset(pin, 0, sizeof(pin));
            }
        }
        /* PIN unlock — write PIN to unlock device */
        if (param->write.handle == s_pin_unlock_handle) {
            if (param->write.len > 0 && param->write.len <= 8) {
                char pin[9] = {0};
                memcpy(pin, param->write.value, param->write.len);
                pin[param->write.len] = '\0';
                if (quartz_wallet_check_pin(pin) == QZ_WALLET_OK) {
                    s_device_unlocked = true;
                    quartz_wallet_reset_pin_attempts();
                    ESP_LOGI(TAG, "Device unlocked via BLE PIN");
                } else {
                    bool wiped = quartz_wallet_record_failed_pin();
                    if (wiped) {
                        s_device_unlocked = false;
                        ESP_LOGE(TAG, "Device wiped after 10 failed PIN attempts");
                    }
                }
                memset(pin, 0, sizeof(pin));
            }
        }
        /* v089.9: WiFi provisioning loop — SSID then password, reboot applies */
        if (param->write.handle == s_wifi_ssid_handle) {
            if (param->write.len > 0 && param->write.len < 33) {
                char ssid[33] = {0};
                memcpy(ssid, param->write.value, param->write.len);
                quartz_wifi_set_credentials(ssid, NULL);
                ESP_LOGI(TAG, "WiFi SSID staged over BLE (%d bytes)", param->write.len);
            }
        }
        if (param->write.handle == s_wifi_pass_handle) {
            if (param->write.len > 0 && param->write.len < 65) {
                char pass[65] = {0};
                memcpy(pass, param->write.value, param->write.len);
                quartz_wifi_set_credentials(NULL, pass);
                ESP_LOGI(TAG, "WiFi password received over BLE — credentials committed");
            }
        }
        if (param->write.handle == s_reboot_handle) {
            if (param->write.len >= 1) {
                ESP_LOGW(TAG, "Reboot requested over BLE — applying WiFi + restarting");
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
        }
        /* v089.12: relay command (0A0D) — CLI syntax, shared parser with
         * serial console. Bonded+encrypted writes only. 'pin <gpio>' and
         * 'test' have the same semantics as the CLI (reboot / ~3s block). */
        if (param->write.handle == s_relay_cmd_handle) {
            if (param->write.len > 0 && param->write.len < 96) {
                char rcmd[96] = {0};
                memcpy(rcmd, param->write.value, param->write.len);
                rcmd[strcspn(rcmd, "\r\n")] = '\0';
                if (!quartz_pay_is_initialized())
                    quartz_pay_init(quartz_wallet_get_address());   /* once: re-init disarms */
                char rreply[192];
                quartz_pay_relay_cmd(rcmd, rreply, sizeof(rreply));
                ESP_LOGI(TAG, "Relay over BLE: '%s' → %s", rcmd, rreply);
            }
        }
        break;

    default:
        break;
    }
}

static bool s_ble_active = false;          /* v083: pair window state */
static esp_timer_handle_t s_pair_timer = NULL;

/* v089.5: per-step bring-up with error codes + clean teardown. A bare
 * "return" on failure used to leave the board BLE-dead forever — the
 * unconfirmed banner's kick was a no-op and the app never saw the miner. */
static bool ble_stack_bringup(void) {
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t e;
    if ((e = esp_bt_controller_init(&bt_cfg)) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed: %s", esp_err_to_name(e));
        return false;
    }
    if ((e = esp_bt_controller_enable(ESP_BT_MODE_BLE)) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed: %s", esp_err_to_name(e));
        esp_bt_controller_deinit();
        return false;
    }
    if ((e = esp_bluedroid_init()) != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(e));
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return false;
    }
    if ((e = esp_bluedroid_enable()) != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(e));
        esp_bluedroid_deinit();
        esp_bt_controller_disable();
        esp_bt_controller_deinit();
        return false;
    }
    return true;
}

void quartz_ble_init(void) {
    if (s_ble_active) return;   /* v089.5: idempotent */
    s_advertising = false;

    for (int attempt = 1; attempt <= 3; attempt++) {
        if (attempt > 1) vTaskDelay(pdMS_TO_TICKS(150));
        ESP_LOGI(TAG, "Starting BLE GATT server (Bluedroid) — attempt %d/3", attempt);
        if (!ble_stack_bringup()) continue;

        /* === BLE Security: require bonding for seed characteristics === */
        uint8_t auth_req = ESP_LE_AUTH_REQ_SC_BOND;  /* Secure Connections + Bond */
        esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth_req, sizeof(auth_req));
        uint8_t iocap = ESP_IO_CAP_NONE;  /* No display/keyboard on ESP32 */
        esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof(iocap));
        uint8_t key_size = 16;  /* Max key size */
        esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof(key_size));
        uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof(init_key));
        uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
        esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof(rsp_key));

        esp_ble_gap_register_callback(gap_event_handler);
        esp_ble_gatts_register_callback(gatts_event_handler);
        esp_ble_gatts_app_register(0);

        esp_ble_gap_set_device_name("Quartz-Miner");

        ESP_LOGI(TAG, "BLE security: bonding required for seed/confirm characteristics");
        s_ble_active = true;
        return;
    }
    ESP_LOGE(TAG, "BLE init failed 3/3 — the unconfirmed banner will keep retrying");
}

/* ---- v083: pair-mode window (BLE on demand; mining keeps full radio after) ---- */

bool quartz_ble_is_active(void) { return s_ble_active; }

void quartz_ble_stop(void) {
    if (!s_ble_active) return;
    s_ble_active = false;
    s_advertising = false;   /* v087 */
    /* v089.7: close a live link before tearing down the stack, and clear
     * the connection state so update_stats stops notifying a ghost */
    if (s_connected && s_gatts_if) {
        esp_ble_gatts_close(s_gatts_if, s_conn_handle);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_connected = false;
    s_conn_handle = 0xFFFF;
    if (s_pair_timer) esp_timer_stop(s_pair_timer);
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();
    ESP_LOGI(TAG, "BLE stopped");
}

bool quartz_ble_is_advertising(void) {
    return s_ble_active && s_advertising && !s_connected;
}

void quartz_ble_kick_adv(void) {
    /* v087: self-heal — BLE up but not advertising → re-issue payload
     * configs (ADV_DATA_SET_COMPLETE starts advertising again).
     * v089.5: BLE never came up → re-run the FULL init, rate-limited to
     * every 30s. The old 3-kick give-up masked dead-BLE boards forever:
     * the banner said "NOT advertising" while this call no-opped. */
    if (s_connected) return;
    if (!s_ble_active) {
        static int64_t last_init_try_us = -30000000LL;
        int64_t now_us = esp_timer_get_time();
        if (now_us - last_init_try_us < 30000000LL) return;
        last_init_try_us = now_us;
        ESP_LOGW(TAG, "BLE down — full re-init (banner self-heal)");
        quartz_ble_init();
        return;
    }
    if (s_advertising) return;
    if (!s_adv_allowed) return;   /* wallet not ready — nothing to serve yet */
    esp_ble_gap_config_adv_data(&s_adv_data_adv);
    esp_ble_gap_config_adv_data(&s_adv_data);
}

static void pair_window_end_cb(void *arg) {
    /* v089.9: phone still connected (mid WiFi-provisioning or watching
     * stats) — extend the window instead of tearing the stack down
     * under a live link */
    if (s_connected) {
        ESP_LOGI(TAG, "Pair window: phone still connected — extending 5 min");
        esp_timer_start_once(s_pair_timer, 300ULL * 1000000ULL);
        return;
    }
    quartz_ble_stop();
    quartz_wifi_set_full_power();
    ESP_LOGI(TAG, "Pair window closed — BLE off, WiFi back to full power");
}

void quartz_ble_pair_window_start(uint32_t seconds) {
    if (!s_ble_active) return;   /* quartz_ble_init() must have run */
    s_adv_allowed = true;        /* v089.8: 'ble on' / post-confirm window always serves */
    if (!s_pair_timer) {
        const esp_timer_create_args_t args = {
            .callback = pair_window_end_cb,
            .name = "qz_ble_win",
        };
        esp_timer_create(&args, &s_pair_timer);
    }
    if (s_pair_timer) {
        esp_timer_stop(s_pair_timer);
        esp_timer_start_once(s_pair_timer, (uint64_t)seconds * 1000000ULL);
    }
}

void quartz_ble_update_stats(uint32_t hash_count, uint32_t hash_rate,
                             uint32_t blocks_found, uint32_t uptime) {
    s_stats.hash_count = hash_count;
    s_stats.hash_rate = hash_rate;
    s_stats.blocks_found = blocks_found;
    s_stats.uptime = uptime;

    if (s_connected && s_gatts_if) {
        /* Notify stats characteristic */
        esp_ble_gatts_send_indicate(s_gatts_if, s_conn_handle,
            s_stats_handle, sizeof(struct mining_stats),
            (uint8_t*)&s_stats, false);
    }
}

void quartz_ble_set_address(const char *address) {
    if (address) {
        strncpy(s_address, address, sizeof(s_address) - 1);
        s_address[sizeof(s_address) - 1] = '\0';
    }
    /* v0894: push immediately (see set_seed_phrase) */
    if (s_addr_handle) {
        esp_ble_gatts_set_attr_value(s_addr_handle,
            strlen(s_address), (uint8_t*)s_address);
    }
}

bool quartz_ble_is_connected(void) {
    return s_connected;
}

void quartz_ble_set_seed_phrase(const char words[12][12]) {
    memcpy(s_seed_phrase, words, sizeof(s_seed_phrase));
    s_seed_available = true;
    s_seed_confirmed = false;
    /* v089.8: wallet exists — unlock advertising (fresh-boot race fix:
     * pairing before this point read zero words and dead-ended the app) */
    if (!s_adv_allowed) {
        s_adv_allowed = true;
        ESP_LOGI(TAG, "Wallet ready — BLE advertising unlocked");
        esp_ble_gap_config_adv_data(&s_adv_data_adv);
        esp_ble_gap_config_adv_data(&s_adv_data);
    }
    /* v0894: push to the GATT stack immediately — refresh-on-read alone
     * lags one read behind (first read still serves the stale copy) */
    if (s_seed_handle) {
        esp_ble_gatts_set_attr_value(s_seed_handle,
            sizeof(s_seed_phrase), (uint8_t*)s_seed_phrase);
    }
    ESP_LOGI(TAG, "Seed phrase loaded for BLE provisioning (read once)");
}

bool quartz_ble_is_seed_confirmed(void) {
    return s_seed_confirmed;
}

bool quartz_ble_is_unlocked(void) {
    return s_device_unlocked;
}

void quartz_ble_get_pin_status(bool *has_pin, uint8_t *attempts_left, bool *unlocked) {
    *has_pin = quartz_wallet_has_pin();
    *attempts_left = 10 - quartz_wallet_pin_attempts();
    *unlocked = s_device_unlocked;
    /* Update BLE status buffer */
    s_pin_status_buf[0] = *has_pin ? 1 : 0;
    s_pin_status_buf[1] = *attempts_left;
    s_pin_status_buf[2] = *unlocked ? 1 : 0;
}

void quartz_ble_set_pin(const char *pin) {
    quartz_wallet_set_pin(pin);
    s_device_unlocked = true;  /* just set it, device stays unlocked */
}

void quartz_ble_unlock(const char *pin) {
    if (quartz_wallet_check_pin(pin) == QZ_WALLET_OK) {
        s_device_unlocked = true;
        quartz_wallet_reset_pin_attempts();
    } else {
        quartz_wallet_record_failed_pin();
    }
}

#endif /* ESP_PLATFORM */
