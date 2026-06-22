// ================================================================
// ESP Ethereum Transaction Signer — firmware
// ================================================================
//
// Holds a secp256k1 private key in EEPROM (flash-backed).
// Signs 32-byte hashes over USB serial after physical confirmation.
// Derives and displays the Ethereum address on an SSD1306 OLED.
//
// Hardware features are toggled in include/config.h:
//   HAS_OLED     – SSD1306 0.96" I²C display
//   HAS_BUTTONS  – tactile confirm / reject buttons
//   HAS_LED      – status LED
//
// With no hardware attached the device runs headless and responds
// to serial commands.  Signing auto-confirms after a short delay
// when buttons are absent (for bringup — wire buttons for prod).

#include <Arduino.h>
#include <EEPROM.h>
#include <ArduinoJson.h>
#include <secp256k1.h>
#include <secp256k1_recovery.h>
#include "keccak256.h"
#include "config.h"

#if HAS_OLED
  #include <Wire.h>
  #include <Adafruit_GFX.h>
  #include <Adafruit_SSD1306.h>
#endif

// ================================================================
// Platform RNG
// ================================================================
// ESP8266: os_random() reads the hardware RNG register directly
// (no WiFi needed).  ESP32: esp_random() uses the TRNG.

#if defined(ESP8266)
  #include <user_interface.h>
  #define PLATFORM_RNG os_random
#else
  #define PLATFORM_RNG esp_random
#endif

// ================================================================
// Globals
// ================================================================

static secp256k1_context* g_ctx      = nullptr;
static uint8_t            g_privkey[32];
static bool               g_has_key  = false;
static char               g_address[43];   // "0x" + 40 hex
static uint8_t            g_pubkey[65];     // 0x04 || x(32) || y(32)

#if HAS_OLED
static Adafruit_SSD1306   g_display(SCREEN_WIDTH, SCREEN_HEIGHT,
                                    &Wire, OLED_RESET);
static bool               g_oled_ok = false;
#endif

// ================================================================
// Forward declarations
// ================================================================

static void oled_setup();
static void oled_show(const char* line1, const char* line2 = nullptr,
                      const char* line3 = nullptr, const char* line4 = nullptr);
static void oled_show_ready();
static void oled_show_sign_request(const char* to, const char* value_eth,
                                   const char* gas_eth, uint32_t nonce,
                                   uint32_t chain_id);

static bool load_key();
static void save_key();
static bool generate_key(secp256k1_context* vctx);
static void derive_address();

static void process_command(const String& json);
static void cmd_ping();
static void cmd_get_address();
static void cmd_sign(const JsonObject& root);
static bool wait_for_confirm(unsigned long timeout_ms);

static void rng_fill(uint8_t* buf, size_t len);
static void led_blink(int count, int period_ms);
static String hex_encode(const uint8_t* data, size_t len);

// ================================================================
// Setup
// ================================================================

void setup() {
    Serial.begin(SERIAL_BAUD);
    delay(100);          // let UART settle; also feeds WDT

    // --- GPIO ---
#if HAS_LED
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, LOW);
#endif
#if HAS_BUTTONS
    pinMode(BTN_CONFIRM, INPUT_PULLUP);
    pinMode(BTN_REJECT,  INPUT_PULLUP);
#endif

    // --- OLED ---
#if HAS_OLED
    oled_setup();
#endif

    // --- Load or generate private key ---
    EEPROM.begin(EEPROM_SIZE);
    g_has_key = load_key();

    if (!g_has_key) {
        oled_show("Generating key...");

        // Feed WDT before long C call
        yield();
        secp256k1_context* vctx =
            secp256k1_context_create(SECP256K1_CONTEXT_NONE);
        yield();
        if (!vctx) {
            Serial.println(F("{\"ok\":false,\"error\":\"OOM at keygen\"}"));
            oled_show("FATAL: OOM", "keygen context");
            while (1) { delay(500); }
        }

        if (!generate_key(vctx)) {
            Serial.println(F("{\"ok\":false,\"error\":\"keygen failed\"}"));
            oled_show("KEY GEN FAILED");
            secp256k1_context_destroy(vctx);
            while (1) { delay(500); }
        }
        secp256k1_context_destroy(vctx);

        save_key();
        g_has_key = true;
        oled_show("Key generated", "Saved to EEPROM");
        delay(800);
    }

    // --- Create signing context ---
    yield();
    g_ctx = secp256k1_context_create(SECP256K1_CONTEXT_SIGN);
    yield();
    if (!g_ctx) {
        Serial.print(F("{\"ok\":false,\"error\":\"OOM sign ctx, heap="));
        Serial.print(ESP.getFreeHeap());
        Serial.println(F("\"}"));
        oled_show("FATAL: OOM", "signing context",
                  "Free heap:", String(ESP.getFreeHeap()).c_str());
        while (1) { delay(500); }
    }

    // Blind context with fresh entropy (side-channel hardening)
    uint8_t seed[32];
    rng_fill(seed, sizeof(seed));
    (void)secp256k1_context_randomize(g_ctx, seed);

    // --- Derive Ethereum address ---
    derive_address();
    oled_show_ready();

    Serial.print(F("{\"ok\":true,\"msg\":\"ready\",\"address\":\""));
    Serial.print(g_address);
    Serial.println(F("\"}"));

#if HAS_LED
    led_blink(2, LED_BLINK_SLOW_MS);
#endif
}

// ================================================================
// Main loop
// ================================================================

void loop() {
    static String line;
    line.reserve(256);

    while (Serial.available()) {
        char c = (char)Serial.read();
        if (c == '\n') {
            line.trim();
            if (line.length() > 0)
                process_command(line);
            line = "";
        } else if (c != '\r') {
            line += c;
        }
        if (line.length() > 1024)
            line = "";
    }

    delay(1);  // feed WDT, yield to background tasks
}

// ================================================================
// OLED helpers (compiled only when HAS_OLED)
// ================================================================

#if HAS_OLED

static void oled_setup() {
    Wire.begin(I2C_SDA, I2C_SCL);
    Wire.setClock(400000);
    g_oled_ok = g_display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
    if (g_oled_ok) {
        g_display.clearDisplay();
        g_display.setTextSize(1);
        g_display.setTextColor(SSD1306_WHITE);
        g_display.setCursor(0, 0);
        g_display.println(F("Booting..."));
        g_display.display();
    }
}

static void oled_show(const char* l1, const char* l2,
                       const char* l3, const char* l4) {
    if (!g_oled_ok) return;
    g_display.clearDisplay();
    g_display.setCursor(0, 0);
    g_display.println(l1);
    if (l2) g_display.println(l2);
    if (l3) g_display.println(l3);
    if (l4) g_display.println(l4);
    g_display.display();
}

static void oled_show_ready() {
    if (!g_oled_ok) return;
    g_display.clearDisplay();
    g_display.setCursor(0, 0);
    g_display.println(F("ETH SIGNER READY"));
    g_display.println();
    g_display.print(F("Addr: "));
    for (int i = 0; i < 10 && g_address[i]; i++)
        g_display.print(g_address[i]);
    g_display.println();
    g_display.println();
    g_display.println(F("Waiting for cmd..."));
    g_display.display();
}

static void oled_show_sign_request(const char* to, const char* value_eth,
                                   const char* gas_eth, uint32_t nonce,
                                   uint32_t chain_id) {
    if (!g_oled_ok) return;
    g_display.clearDisplay();
    g_display.setCursor(0, 0);

    g_display.println(F("--- SIGN TX? ---"));

    g_display.print(F("To: "));
    size_t tl = strlen(to);
    if (tl > 14) {
        for (size_t i = 0; i < 6; i++)   g_display.print(to[i]);
        g_display.print(F("..."));
        for (size_t i = tl - 4; i < tl; i++) g_display.print(to[i]);
    } else {
        g_display.print(to);
    }
    g_display.println();

    g_display.print(F("Val: "));  g_display.print(value_eth);
    g_display.println(F(" ETH"));
    g_display.print(F("Gas: "));  g_display.print(gas_eth);
    g_display.println(F(" ETH"));
    g_display.print(F("Nonce:")); g_display.print(nonce);
    g_display.print(F(" Ch:"));   g_display.println(chain_id);

    g_display.println();
    g_display.println(F("CONFIRM      REJECT"));
    g_display.display();
}

#else  // !HAS_OLED

static void oled_setup() {}
static void oled_show(const char*, const char*, const char*, const char*) {}
static void oled_show_ready() {}
static void oled_show_sign_request(const char*, const char*, const char*,
                                    uint32_t, uint32_t) {}

#endif  // HAS_OLED

// ================================================================
// Key management
// ================================================================

static bool load_key() {
    if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC)
        return false;
    for (int i = 0; i < 32; i++)
        g_privkey[i] = EEPROM.read(EEPROM_KEY_ADDR + i);
    return true;
}

static void save_key() {
    EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
    for (int i = 0; i < 32; i++)
        EEPROM.write(EEPROM_KEY_ADDR + i, g_privkey[i]);
    EEPROM.commit();
}

static bool generate_key(secp256k1_context* vctx) {
    uint32_t extra = (uint32_t)(micros() & 0xFFFFFFFF);
#if defined(ESP8266)
    extra ^= (uint32_t)(system_get_rtc_time() & 0xFFFFFFFF);
#endif

    for (int attempt = 0; attempt < 256; attempt++) {
        yield();  // feed WDT
        for (int i = 0; i < 32; i += 4) {
            uint32_t r = PLATFORM_RNG() ^ extra;
            extra = (extra << 13) | (extra >> 19);
            g_privkey[i]   = r & 0xFF;
            g_privkey[i+1] = (r >> 8) & 0xFF;
            g_privkey[i+2] = (r >> 16) & 0xFF;
            g_privkey[i+3] = (r >> 24) & 0xFF;
        }
        if (secp256k1_ec_seckey_verify(vctx, g_privkey))
            return true;
    }
    return false;
}

// ================================================================
// Address derivation
// ================================================================

static void derive_address() {
    secp256k1_pubkey pk;
    if (!secp256k1_ec_pubkey_create(g_ctx, &pk, g_privkey)) {
        memset(g_address, 0, sizeof(g_address));
        memset(g_pubkey,  0, sizeof(g_pubkey));
        return;
    }

    size_t len = sizeof(g_pubkey);
    secp256k1_ec_pubkey_serialize(g_ctx, g_pubkey, &len, &pk,
                                   SECP256K1_EC_UNCOMPRESSED);
    // g_pubkey is now: 0x04 || x(32) || y(32)

    // Ethereum address = last 20 bytes of keccak256(x || y)
    uint8_t hash[32];
    keccak256(g_pubkey + 1, 64, hash);

    g_address[0] = '0';
    g_address[1] = 'x';
    for (int i = 0; i < 20; i++) {
        auto hex = [](uint8_t nib) -> char {
            return nib < 10 ? '0' + nib : 'a' + (nib - 10);
        };
        g_address[2 + i*2]     = hex(hash[12 + i] >> 4);
        g_address[2 + i*2 + 1] = hex(hash[12 + i] & 0x0F);
    }
    g_address[42] = '\0';
}

// ================================================================
// Serial JSON-RPC protocol
// ================================================================

static void process_command(const String& json) {
    StaticJsonDocument<512> doc;
    if (deserializeJson(doc, json)) {
        Serial.println(F("{\"ok\":false,\"error\":\"parse_error\"}"));
        return;
    }
    const char* cmd = doc["cmd"];
    if (!cmd) {
        Serial.println(F("{\"ok\":false,\"error\":\"missing cmd\"}"));
        return;
    }

    if      (strcmp(cmd, "ping")        == 0) cmd_ping();
    else if (strcmp(cmd, "get_address") == 0) cmd_get_address();
    else if (strcmp(cmd, "sign")        == 0) cmd_sign(doc.as<JsonObject>());
    else
        Serial.println(F("{\"ok\":false,\"error\":\"unknown command\"}"));
}

static void cmd_ping() {
    Serial.println(F("{\"ok\":true,\"msg\":\"pong\"}"));
#if HAS_LED
    led_blink(1, 50);
#endif
}

static void cmd_get_address() {
    StaticJsonDocument<256> doc;
    doc[F("ok")]      = true;
    doc[F("address")] = g_address;
    doc[F("pubkey")]  = hex_encode(g_pubkey, 65);
    serializeJson(doc, Serial);
    Serial.println();
#if HAS_LED
    led_blink(1, 50);
#endif
}

static void cmd_sign(const JsonObject& root) {
    // --- Validate the 32-byte hash ---
    const char* hx = root["hash"];
    if (!hx || strlen(hx) != 66 || hx[0] != '0' || hx[1] != 'x') {
        Serial.println(F("{\"ok\":false,\"error\":\"invalid hash\"}"));
        return;
    }

    uint8_t hash[32];
    for (int i = 0; i < 32; i++) {
        auto nib = [](char c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        hash[i] = (nib(hx[2 + i*2]) << 4) | nib(hx[2 + i*2 + 1]);
    }

    // --- Show transaction details ---
    JsonObject disp = root["display"];
    const char* to        = disp["to"]        | "";
    const char* value_eth = disp["value_eth"] | "0";
    const char* gas_eth   = disp["gas_eth"]   | "0";
    uint32_t    nonce     = disp["nonce"]     | 0;
    uint32_t    chain_id  = disp["chain_id"]  | 1;

    oled_show_sign_request(to, value_eth, gas_eth, nonce, chain_id);

    // --- Wait for physical confirmation ---
#if HAS_LED
    digitalWrite(LED_PIN, HIGH);
#endif

    bool confirmed;
#if HAS_BUTTONS
    confirmed = wait_for_confirm(SIGN_TIMEOUT_MS);
#else
    // No buttons wired — delay then auto-confirm.
    // WARNING: wire buttons before using with real value!
    delay(5000);
    confirmed = true;
#endif

#if HAS_LED
    digitalWrite(LED_PIN, LOW);
#endif

    if (!confirmed) {
        oled_show("REJECTED", "TX not signed");
        delay(1500);
        oled_show_ready();
        Serial.println(F("{\"ok\":false,\"error\":\"rejected\"}"));
        return;
    }

    // --- Sign ---
    secp256k1_ecdsa_recoverable_signature sig;
    if (!secp256k1_ecdsa_sign_recoverable(g_ctx, &sig, hash, g_privkey,
                                           nullptr, nullptr)) {
        Serial.println(F("{\"ok\":false,\"error\":\"signing failed\"}"));
        oled_show("SIGN FAILED");
        return;
    }

    uint8_t compact[64];
    int recid = 0;
    secp256k1_ecdsa_recoverable_signature_serialize_compact(
        g_ctx, compact, &recid, &sig);

    // Re-blind context after every signature (defense-in-depth)
    uint8_t sd[32];
    rng_fill(sd, sizeof(sd));
    (void)secp256k1_context_randomize(g_ctx, sd);

    // --- Respond ---
    StaticJsonDocument<384> doc;
    doc[F("ok")]      = true;
    doc[F("r")]       = hex_encode(compact, 32);
    doc[F("s")]       = hex_encode(compact + 32, 32);
    doc[F("yParity")] = recid;
    serializeJson(doc, Serial);
    Serial.println();

    oled_show("SIGNED", "TX sent to host");
    delay(1500);
    oled_show_ready();

#if HAS_LED
    led_blink(2, LED_BLINK_SLOW_MS);
#endif
}

// ================================================================
// Button handling (compiled only when HAS_BUTTONS)
// ================================================================

#if HAS_BUTTONS

static bool wait_for_confirm(unsigned long timeout_ms) {
    unsigned long start = millis();
    bool last_c = true, last_r = true;   // pulled-up = HIGH

    while (millis() - start < timeout_ms) {
        bool c = digitalRead(BTN_CONFIRM);   // LOW = pressed
        bool r = digitalRead(BTN_REJECT);

        // Falling-edge detect on confirm
        if (c == LOW && last_c == HIGH) {
            delay(30);   // debounce
            if (digitalRead(BTN_CONFIRM) == LOW)
                return true;
        }
        // Falling-edge detect on reject
        if (r == LOW && last_r == HIGH) {
            delay(30);
            if (digitalRead(BTN_REJECT) == LOW)
                return false;
        }
        last_c = c;
        last_r = r;

#if HAS_LED
        digitalWrite(LED_PIN,
            ((millis() - start) / LED_BLINK_FAST_MS) % 2 == 0);
#endif
        delay(10);
    }
    return false;   // timeout = reject
}

#else  // !HAS_BUTTONS

static bool wait_for_confirm(unsigned long) { return true; }

#endif  // HAS_BUTTONS

// ================================================================
// Utilities
// ================================================================

static void rng_fill(uint8_t* buf, size_t len) {
    for (size_t i = 0; i < len; i += 4) {
        uint32_t r = PLATFORM_RNG();
        buf[i]   = r & 0xFF;
        if (i+1 < len) buf[i+1] = (r >> 8) & 0xFF;
        if (i+2 < len) buf[i+2] = (r >> 16) & 0xFF;
        if (i+3 < len) buf[i+3] = (r >> 24) & 0xFF;
    }
}

static String hex_encode(const uint8_t* data, size_t len) {
    String s;
    s.reserve(2 + len * 2);
    s = "0x";
    for (size_t i = 0; i < len; i++) {
        auto hex = [](uint8_t nib) -> char {
            return nib < 10 ? '0' + nib : 'a' + (nib - 10);
        };
        char buf[3] = { hex(data[i] >> 4), hex(data[i] & 0x0F), '\0' };
        s += buf;
    }
    return s;
}

#if HAS_LED
static void led_blink(int count, int period_ms) {
    int half = period_ms / 2;
    for (int i = 0; i < count; i++) {
        digitalWrite(LED_PIN, HIGH);
        delay(half);
        digitalWrite(LED_PIN, LOW);
        if (i < count - 1) delay(half);
    }
}
#endif
