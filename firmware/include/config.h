#pragma once

// ================================================================
// ESP Ethereum Transaction Signer — hardware configuration
// ================================================================
//
// Board selection:  define exactly ONE of these via build_flags in
// platformio.ini (already done for you).  If you're compiling
// outside PlatformIO, uncomment one line below.
//
//   ESP8266_BOARD  – NodeMCU / Wemos D1 Mini
//   ESP32_BOARD    – classic ESP32-DevKitC / NodeMCU-32S
//   ESP32S3_BOARD  – ESP32-S3-DevKitC-1

// ================================================================
// 1. Optional hardware — set to 1 when wired
// ================================================================

#define HAS_OLED      0   // SSD1306 0.96" 128×64 I²C OLED
#define HAS_BUTTONS   0   // confirm + reject tactile push-buttons
#define HAS_LED       0   // status LED + current-limiting resistor

// ================================================================
// 2. Pin mapping (board-specific)
// ================================================================

#if defined(ESP8266_BOARD)
  // --- NodeMCU / Wemos D1 Mini (ESP8266) ---
  #define I2C_SDA         4   // D2
  #define I2C_SCL         5   // D1
  #define BTN_CONFIRM     12  // D6
  #define BTN_REJECT      13  // D7
  #define LED_PIN         14  // D5

#elif defined(ESP32_BOARD)
  // --- Classic ESP32-DevKitC / NodeMCU-32S ---
  #define I2C_SDA         21
  #define I2C_SCL         22
  #define BTN_CONFIRM     25
  #define BTN_REJECT      26
  #define LED_PIN         27

#elif defined(ESP32S3_BOARD)
  // --- ESP32-S3-DevKitC-1 ---
  #define I2C_SDA         21
  #define I2C_SCL         22
  #define BTN_CONFIRM     25
  #define BTN_REJECT      26
  #define LED_PIN         27

#else
  #error "No board selected.  Define ESP8266_BOARD, ESP32_BOARD, or ESP32S3_BOARD."
#endif

// ================================================================
// 3. Shared constants
// ================================================================

#define OLED_RESET       -1    // no dedicated reset pin
#define SCREEN_WIDTH     128
#define SCREEN_HEIGHT    64
#define OLED_I2C_ADDR    0x3C

#define SERIAL_BAUD      115200
#define SIGN_TIMEOUT_MS  30000   // auto-reject after 30 s
#define LED_BLINK_FAST_MS 150
#define LED_BLINK_SLOW_MS 600

// EEPROM layout (flash-backed on ESP8266, NVS-backed on ESP32)
#define EEPROM_SIZE       64
#define EEPROM_MAGIC_ADDR 0
#define EEPROM_KEY_ADDR   1
#define EEPROM_MAGIC      0xAB
