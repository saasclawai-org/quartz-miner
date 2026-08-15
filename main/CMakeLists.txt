idf_component_register(
    SRCS
        "quartz.c"
        "main.c"
        "quartz_attest.c"
        "quartz_pool.c"
        "quartz_supply_chain.c"
        "quartz_entropy.c"
        "quartz_storage.c"
        "quartz_wallet.c"
        "quartz_crypto.c"
        "quartz_bip39.c"
        "quartz_display.c"
        "quartz_wifi.c"
        "quartz_puf.c"
        "quartz_wots.c"
        "quartz_pay.c"
        "quartz_qr.c"
        "qrcodegen.c"
        "quartz_agent.c"
        "quartz_sx1276.c"
        "quartz_ble.c"
        "ed25519-lib/add_scalar.c"
        "ed25519-lib/fe.c"
        "ed25519-lib/ge.c"
        "ed25519-lib/keypair.c"
        "ed25519-lib/sc.c"
        "ed25519-lib/sign.c"
        "ed25519-lib/verify.c"
        "ed25519-lib/sha512_mbedtls.c"
    INCLUDE_DIRS "." "ed25519-lib"
    REQUIRES mbedtls fatfs nvs_flash esp_driver_spi esp_timer esp_event esp_wifi bt esp_adc efuse driver
)

# ESP32 (M5Stack) has display+buttons, ESP32-S3 (T3) is headless
if(CONFIG_IDF_TARGET_ESP32)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE QUARTZ_HAS_DISPLAY)
endif()

# Relax warnings for development build
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=all
    -Wno-implicit-function-declaration
    -Wno-incompatible-pointer-types
    -Wno-int-conversion
    -Wno-return-type
    -Wno-format
    -Wno-unused-variable
    -Wno-unused-function
    -Wno-unused-but-set-variable
)
