/**
 * quartz_ble.c — NimBLE GATT server for Quartz ESP32
 * Exposes mining stats and wallet address over BLE
 */

#include "quartz_ble.h"

#ifdef ESP_PLATFORM
#include "esp_log.h"
#include "nimble/NimBLEDevice.h"
#include <string.h>
#include <stdio.h>

static const char *TAG = "QZ.BLE";

/* Stats data (packed for BLE transfer) */
struct __attribute__((packed)) mining_stats {
    uint32_t hash_count;
    uint32_t hash_rate;
    uint32_t blocks_found;
    uint32_t uptime;
};

static mining_stats s_stats = {0};
static char s_address[64] = {0};
static bool s_connected = false;

/* --- Custom Service + Characteristics --- */

class QuartzStatsCharacteristic : public NimBLECharacteristic {
public:
    void onRead(NimBLEConnInfo& connInfo) override {
        setValue((uint8_t*)&s_stats, sizeof(s_stats));
    }
};

class QuartzAddressCharacteristic : public NimBLECharacteristic {
public:
    void onRead(NimBLEConnInfo& connInfo) override {
        setValue(s_address);
    }
};

class QuartzServerCallbacks : public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
        ESP_LOGI(TAG, "Phone connected to BLE");
        s_connected = true;
    }
    void onDisconnect(NimBLEServer* server, NimBLEConnInfo& connInfo, int reason) override {
        ESP_LOGI(TAG, "Phone disconnected from BLE (reason %d)", reason);
        s_connected = false;
    }
};

static QuartzStatsCharacteristic* s_stats_char = nullptr;
static QuartzAddressCharacteristic* s_addr_char = nullptr;

void quartz_ble_init(void) {
    ESP_LOGI(TAG, "Starting BLE GATT server (NimBLE)");

    NimBLEDevice::init("Quartz-Miner");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);  /* Max power */

    NimBLEServer* server = NimBLEDevice::createServer();
    server->setCallbacks(new QuartzServerCallbacks());

    /* Mining stats service */
    NimBLEService* service = server->createService(QUARTZ_BLE_SERVICE_UUID);

    /* Stats characteristic — readable, notifies on update */
    s_stats_char = (QuartzStatsCharacteristic*)service->createCharacteristic(
        QUARTZ_CHAR_STATS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );
    s_stats_char->setValue((uint8_t*)&s_stats, sizeof(s_stats));

    /* Wallet address characteristic — readable */
    s_addr_char = (QuartzAddressCharacteristic*)service->createCharacteristic(
        QUARTZ_CHAR_ADDRESS_UUID,
        NIMBLE_PROPERTY::READ
    );
    s_addr_char->setValue(s_address);

    service->start();

    /* Advertising */
    NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
    adv->addServiceUUID(QUARTZ_BLE_SERVICE_UUID);
    adv->setScanResponse(true);
    adv->start();

    ESP_LOGI(TAG, "BLE advertising as \"Quartz-Miner\"");
}

void quartz_ble_update_stats(uint32_t hash_count, uint32_t hash_rate,
                             uint32_t blocks_found, uint32_t uptime) {
    s_stats.hash_count = hash_count;
    s_stats.hash_rate = hash_rate;
    s_stats.blocks_found = blocks_found;
    s_stats.uptime = uptime;

    if (s_connected && s_stats_char) {
        s_stats_char->setValue((uint8_t*)&s_stats, sizeof(s_stats));
        s_stats_char->notify();
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

#endif /* ESP_PLATFORM */