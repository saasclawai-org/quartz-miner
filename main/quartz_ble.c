/**
 * quartz_ble.c — Bluedroid GATT server for Quartz ESP32
 * Exposes mining stats and wallet address over BLE
 */

#include "quartz_ble.h"
#include "quartz_wallet.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_bt_defs.h"
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

static esp_ble_adv_data_t s_adv_data = {
    .set_scan_rsp = true,
    .include_name = true,
    .include_txpower = false,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 16,
    .p_service_uuid = s_service_uuid128,
    .flag = (ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT),
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
    QUARTZ_IDX_NB,
};

static esp_gatts_attr_db_t s_attr_db[QUARTZ_IDX_NB] = {
    /* Service */
    [QUARTZ_IDX_SVC] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_service_uuid128, ESP_GATT_PERM_READ,
         0, 0, NULL}
    },
    /* Stats characteristic declaration */
    [QUARTZ_IDX_STATS_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_NOTIFY}}
    },
    /* Stats characteristic value */
    [QUARTZ_IDX_STATS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_stats_uuid128, ESP_GATT_PERM_READ,
         sizeof(struct mining_stats), sizeof(struct mining_stats), (uint8_t*)&s_stats}
    },
    /* Address characteristic declaration */
    [QUARTZ_IDX_ADDR_CHAR] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_READ}}
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
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_READ}}
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
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE}}
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
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_WRITE}}
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
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_WRITE}}
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
        {ESP_UUID_LEN_16, (uint8_t*)&(uint16_t){0x2901}, ESP_GATT_PERM_READ,
         sizeof(uint16_t), sizeof(uint16_t), (uint8_t*)&(uint16_t){ESP_GATT_CHAR_PROP_BIT_READ}}
    },
    /* PIN status value — read: [has_pin, attempts_left, unlocked] */
    [QUARTZ_IDX_PIN_STATUS_VAL] = {
        {ESP_GATT_AUTO_RSP},
        {ESP_UUID_LEN_128, s_pin_status_uuid128, ESP_GATT_PERM_READ,
         sizeof(s_pin_status_buf), sizeof(s_pin_status_buf), s_pin_status_buf}
    },
};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        esp_ble_gap_start_advertising(&s_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
            ESP_LOGE(TAG, "Advertising start failed");
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
            esp_ble_gatts_start_service(param->add_attr_tab.handles[QUARTZ_IDX_SVC]);
            esp_ble_gap_config_adv_data(&s_adv_data);
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
        esp_ble_gap_start_advertising(&s_adv_params);
        break;

    case ESP_GATTS_READ_EVT:
        if (param->read.handle == s_stats_handle + 1) {
            esp_ble_gatts_set_attr_value(s_stats_handle + 1,
                sizeof(struct mining_stats), (uint8_t*)&s_stats);
        }
        if (param->read.handle == s_seed_handle + 1 && s_seed_confirmed) {
            /* Seed already confirmed — return empty */
            uint8_t empty = 0;
            esp_ble_gatts_set_attr_value(s_seed_handle + 1, 0, &empty);
        }
        break;

    case ESP_GATTS_WRITE_EVT:
        if (param->write.handle == s_confirm_handle + 1) {
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
        if (param->write.handle == s_pin_set_handle + 1) {
            if (param->write.len > 0 && param->write.len <= 8) {
                char pin[9] = {0};
                memcpy(pin, param->write.value, param->write.len);
                pin[param->write.len] = '\0';
                quartz_wallet_set_pin(pin);
                memset(pin, 0, sizeof(pin));
            }
        }
        /* PIN unlock — write PIN to unlock device */
        if (param->write.handle == s_pin_unlock_handle + 1) {
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
        break;

    default:
        break;
    }
}

void quartz_ble_init(void) {
    ESP_LOGI(TAG, "Starting BLE GATT server (Bluedroid)");

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    if (esp_bt_controller_init(&bt_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller init failed");
        return;
    }
    if (esp_bt_controller_enable(ESP_BT_MODE_BLE) != ESP_OK) {
        ESP_LOGE(TAG, "BT controller enable failed");
        return;
    }
    if (esp_bluedroid_init() != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid init failed");
        return;
    }
    if (esp_bluedroid_enable() != ESP_OK) {
        ESP_LOGE(TAG, "Bluedroid enable failed");
        return;
    }

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
}

bool quartz_ble_is_connected(void) {
    return s_connected;
}

void quartz_ble_set_seed_phrase(const char words[12][12]) {
    memcpy(s_seed_phrase, words, sizeof(s_seed_phrase));
    s_seed_available = true;
    s_seed_confirmed = false;
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