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
        "quartz_wifi.c"
        "quartz_puf.c"
        "quartz_wots.c"
        "quartz_pay.c"
        "quartz_qr.c"
        "qrcodegen.c"
        "quartz_agent.c"
        "quartz_sx1276.c"
        "quartz_ble.c"
        "quartz_mesh.c"
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

# Per-target display driver:
#   ESP32    (M5Stack Core, ILI9341 SPI TFT + 3 buttons)
#   ESP32-S3 (Heltec V3 / LilyGO T3, SSD1306 128x64 OLED + battery ADC)
#   ESP32-C3 (headless)
if(CONFIG_IDF_TARGET_ESP32)
    target_sources(${COMPONENT_LIB} PRIVATE "quartz_display.c")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE QUARTZ_HAS_DISPLAY)
elseif(CONFIG_IDF_TARGET_ESP32S3)
    target_sources(${COMPONENT_LIB} PRIVATE "quartz_display_s3.c")
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
