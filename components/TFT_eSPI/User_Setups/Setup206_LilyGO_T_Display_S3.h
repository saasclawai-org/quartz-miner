# TFT_eSPI Custom Configuration for LilyGO T-Display S3
# Place this at: components/TFT_eSPI/User_Setups/Setup206_LilyGO_T_Display_S3.h
# And reference via override_path in idf_component.yml

# Target: LilyGO T-Display S3 (ESP32-S3)
# Display: 1.14" ST7789V, 240×135 pixels
# Interface: SPI (4-wire)

# Driver
#define ST7789_DRIVER

# Resolution
#define TFT_WIDTH  240
#define TFT_HEIGHT 135

# Pin configuration (T-Display S3)
#define TFT_MOSI            35
#define TFT_SCLK            36
#define TFT_CS              34
#define TFT_DC              37
#define TFT_RST             38
#define TFT_BL              38  # Backlight (shared with RST on some boards)

# SPI
#define SPI_FREQUENCY       80000000   # 80MHz — T-Display S3 supports this
#define SPI_READ_FREQUENCY  20000000
#define TFT_SPI_MODE        SPI_MODE0

# Rotation — landscape orientation
#define TFT_ROTATION        1

# Color depth
#define TFT_RGB_ORDER       TFT_RGB     # Red and Blue swapped on some panels

# Fonts — embed only what we use to save flash
#define LOAD_GLCD           # Font 1 (small, for stats)
#define LOAD_FONT2          # Font 2 (medium, for amounts)
# Skip fonts 4, 6, 7, 8 to save flash space

# Smooth anti-aliased fonts would need SPIFFS/LittleFS — skip for now

# Touchscreen: none on T-Display S3
#undef  TOUCH_CS

# Don't invert colors
//#define TFT_INVERSION_ON
