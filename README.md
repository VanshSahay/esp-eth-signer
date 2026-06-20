# ESP Ethereum Transaction Signer

A small hardware device that holds a secp256k1 private key, shows you
exactly what you're about to sign on an OLED screen, and only signs an
EIP-1559 transaction after you physically press a button. It talks to
your computer over USB serial. The computer never sees the private key —
it only ever sends a 32-byte hash and receives back `(r, s, yParity)`.

Supports **ESP8266**, **classic ESP32**, and **ESP32-S3**.

## Quick start

### 1. Install PlatformIO

```bash
brew install platformio    # macOS
# or: pip install platformio
```

### 2. Wire the hardware

| ESP8266 pin | ESP32 pin | Connect to |
|---|---|---|
| GPIO4 (D2) | GPIO21 | OLED SDA |
| GPIO5 (D1) | GPIO22 | OLED SCL |
| GPIO12 (D6) | GPIO25 | Confirm button → GND |
| GPIO13 (D7) | GPIO26 | Reject button → GND |
| GPIO14 (D5) | GPIO27 | LED + 220Ω resistor → GND |
| — | — | OLED VCC → 3.3V, GND → GND |

**Buttons**: use `INPUT_PULLUP` — no external resistors needed.  
**OLED**: SSD1306 0.96" 128×64 I²C (most breakouts include pull-ups).  
**I²C address**: `0x3C` (configurable in `config.h`).

### 3. Enable your hardware

Edit `firmware/include/config.h`:

```c
#define HAS_OLED      1   // set to 1 when your OLED is wired
#define HAS_BUTTONS   1   // set to 1 when buttons are wired
#define HAS_LED       1   // set to 1 when LED is wired
```

All three default to `0` — the device works fine headless (serial-only)
without any hardware attached.

### 4. Flash

```bash
cd firmware
pio run -t upload
```

### 5. Install the host tool

```bash
cd host
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

### 6. Sign a transaction

```bash
python sign_client.py \
  --port /dev/cu.usbserial-0001 \
  --rpc https://your-rpc-endpoint \
  --to 0xRecipientAddress \
  --value-eth 0.01
```

Add `--broadcast` to submit to the network.  Omit it to print the raw
signed transaction for inspection first.

Find your serial port: `ls /dev/cu.*` (macOS) or `ls /dev/ttyUSB*` (Linux).

## Serial protocol

Newline-delimited JSON at 115200 baud:

```json
→ {"cmd":"ping"}
← {"ok":true,"msg":"pong"}

→ {"cmd":"get_address"}
← {"ok":true,"address":"0x6cbf...","pubkey":"0x04f786..."}

→ {"cmd":"sign","hash":"0x<32 bytes>","display":{"to":"0x...","value_eth":"0.01","gas_eth":"0.0003","nonce":5,"chain_id":1}}
← {"ok":true,"r":"0x...","s":"0x...","yParity":0}
  or
← {"ok":false,"error":"rejected"}
```

The device only signs whatever 32-byte hash it receives — it has no idea
whether that hash matches the `display` fields.  The host script's job is
to build the transaction correctly and send the right hash.

## Board selection

The default `platformio.ini` targets **ESP8266** (NodeMCU v2). To switch
boards, comment out the `[env:esp8266]` block and uncomment the block for
your board:

| Board | `platformio.ini` section |
|---|---|
| ESP8266 (NodeMCU, D1 Mini) | `[env:esp8266]` |
| Classic ESP32 (DevKitC, NodeMCU-32S) | `[env:esp32]` |
| ESP32-S3 (DevKitC-1) | `[env:esp32s3]` |

Pin mappings for all three boards are in `firmware/include/config.h`.

## Project structure

```
esp-eth-signer/
├── firmware/
│   ├── platformio.ini          # PlatformIO build config
│   ├── include/
│   │   └── config.h            # pin mapping + feature flags
│   ├── src/
│   │   ├── main.cpp            # firmware (keygen, sign, serial)
│   │   ├── keccak256.cpp       # Keccak-256 (Ethereum hash)
│   │   └── keccak256.h
│   ├── lib/
│   │   └── secp256k1/          # vendored secp256k1-embedded
│   │       └── ...               (with ESP8266 patches)
│   └── patches/
│       └── README.md           # why the patches exist
├── host/
│   ├── sign_client.py          # Python host client
│   └── requirements.txt
├── README.md
└── LICENSE
```

## Security

This is a DIY/learning project — not an audited hardware wallet.
Honest limitations:

- **No PIN / passphrase.**  Anyone with physical access and the confirm
  button can sign.  Consider adding a PIN check before `cmd_sign()`
  proceeds if you want a second factor.
- **No anti-glitching or side-channel hardening.**  A motivated attacker
  with physical access and lab equipment can likely extract the key.
  This gap is what dedicated secure-element chips close (see below).
- **Supply-chain trust.**  You're trusting the ESP silicon, the
  bootloader, every vendored library, and your build toolchain.
- **Display trust.**  The OLED shows whatever the host tells it to show.
  "What you see is what you sign" is only as honest as the host script
  — keep it under your control.
- **Test on testnet first.**  Send a test transaction on Sepolia or Holesky
  with throwaway funds before pointing this at mainnet.

### Why no secure-element chip?

The ESP32-S2/S3's Digital Signature peripheral only does RSA, not
secp256k1.  The ATECC608A variant on some ESP32 modules doesn't support
the secp256k1 curve either.  NXP's SE050 does support it, but requires
a sizeable C SDK and hasn't been integrated here.

What this design does instead: the key lives in flash-backed EEPROM (NVS
on ESP32).  The ESP32 family supports **flash encryption + Secure Boot
v2** at the silicon level — once enabled, the private key is encrypted
at rest and the bootloader refuses to run unsigned firmware.  This
project ships with flash encryption **off** so you can iterate.  Enable
it only after your firmware is finalized (this burns eFuses — it's a
one-way operation).

```bash
# ESP32 flash encryption (final step — irreversible):
espefuse.py burn_efuse FLASH_CRYPT_CNT
```

## Bill of materials

| Part | Approx. cost |
|---|---|
| ESP32 dev board (classic or S3) or ESP8266 | $5–12 |
| SSD1306 0.96" 128×64 OLED, I²C | $3–5 |
| 2 × 6mm tactile push buttons | $0.50 |
| 1 × 5mm LED + 220–330Ω resistor | $0.20 |
| Breadboard + jumper wires | $5 |
| USB cable (data-capable) | you probably have one |

**Total: ~$13–22** for a fully exposed prototype.

## ESP8266 notes

The ESP8266 has only 80 KB of DRAM.  Three patches to the secp256k1
library (already applied to the vendored copy in `firmware/lib/`) make
this work:

1. The 80 KB precomputed table is stored in **flash** (`ICACHE_RODATA_ATTR`)
   instead of DRAM.
2. Static precomputation is enabled so the table is pre-built at compile
   time rather than allocated on the stack at runtime.
3. A stack-to-heap fallback in the dynamic build path (dead code when
   static precomp is on, but prevents a crash if someone experiments).

These are documented in `firmware/patches/README.md`.  If you switch to
ESP32 (320 KB+ DRAM) you can revert to the upstream library — none of
these patches are needed there.

## License

MIT — see [LICENSE](LICENSE).
