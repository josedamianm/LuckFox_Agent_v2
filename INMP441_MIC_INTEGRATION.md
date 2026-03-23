# INMP441 Microphone Integration — LuckFox Agent V2
**Document type:** AI Agent instruction set — read completely before making any changes.  
**Scope:** Adds full-duplex audio (INMP441 mic input + MAX98357A speaker output) to the ESP32-C3 ABrobot OLED, and wires the upstream mic audio path into the `http_api_server_v2.py` pipeline on the LuckFox Pico Max.  
**Status:** Hardware validated. Firmware written and reviewed. Python integration pending.

---

## 1. Context and Why This Matters

The existing `CLAUDE.md` (section 2, "Pending") identifies audio input as the **#1 blocker** for the AI voice pipeline:

> "LuckFox Pico Max has NO onboard mic (RV1106 audio ADC pins unpopulated). An external mic is required before audio recording is possible."

This document resolves that blocker. The chosen solution is:

- **INMP441** I2S MEMS microphone wired to the ESP32-C3 ABrobot board
- ESP32-C3 captures audio from the mic and streams it **upstream** to the LuckFox over the **same UART2 link** already used for speaker playback
- LuckFox Python server receives the mic audio, buffers it during `LISTENING` state, and forwards the WAV to the MacBook pipeline for STT

This keeps the audio path entirely on hardware already in the system. No USB mic, no new serial ports, no new processes.

---

## 2. Hardware Summary

### 2.1 Board: ESP32-C3 ABrobot OLED 0.42"

| Spec | Value |
|---|---|
| MCU | ESP32-C3, RISC-V single-core, 160 MHz |
| Flash | 4MB |
| SRAM | 400KB |
| I2S peripherals | 1 × I2S0 (supports full-duplex TX+RX simultaneously) |
| UART peripherals | UART0 (debug), UART1 (Luckfox link) |
| I2C | Hardware I2C (GPIO8/9 are the "official" safe I2C pins) |
| OLED | 0.42" SSD1306, 72×40 visible, wired on GPIO5 (SDA) / GPIO6 (SCL) |
| Safe GPIO (no boot risk) | IO0, IO1, IO3, IO8, IO9, IO10 |
| Strapping pin (caution) | IO2 — must be HIGH at boot |
| JTAG pins (avoid) | IO4, IO5, IO6, IO7 |

### 2.2 Complete Pin Assignment (this project)

| GPIO | Signal | Device | Notes |
|---|---|---|---|
| GPIO0 | I2S BCLK | MAX98357A + INMP441 | Shared clock — both devices |
| GPIO1 | I2S LRC / WS | MAX98357A + INMP441 | Shared word-select — both devices |
| GPIO2 | I2S DOUT | MAX98357A | ⚠ Strapping pin — works if amp doesn't pull LOW at boot |
| GPIO3 | SD / EN | MAX98357A | Amp shutdown control — HIGH = enabled |
| GPIO4 | UART1 RX | LuckFox TX (GPIO42) | Receives audio + commands from LuckFox |
| GPIO5 | OLED SDA | SSD1306 | JTAG pin but works; do not change |
| GPIO6 | OLED SCL | SSD1306 | JTAG pin but works; do not change |
| GPIO7 | UART1 TX | LuckFox RX (GPIO43) | Sends mic data + ACK/STATUS upstream |
| GPIO10 | I2S DIN | INMP441 SD pin | **Only new wire added for mic** |

### 2.3 INMP441 Wiring

| INMP441 Pin | Connect To | Notes |
|---|---|---|
| VDD | ESP32-C3 3V3 | 1.8–3.3V max — NEVER connect to 5V |
| GND | GND | Shared ground |
| BCLK | GPIO0 | Shared with MAX98357A BCLK |
| WS | GPIO1 | Shared with MAX98357A LRC |
| SD | GPIO10 | Only new wire in the system |
| L/R | GND | Selects LEFT channel; WS low = left in Philips I2S |

### 2.4 MAX98357A Wiring (existing — unchanged)

| MAX98357A Pin | Connect To |
|---|---|
| VIN | LuckFox VBUS (5V) — confirmed safe (2.5–5.5V range) |
| GND | GND |
| BCLK | GPIO0 |
| LRC | GPIO1 |
| DIN | GPIO2 |
| SD/EN | GPIO3 |

### 2.5 LuckFox ↔ ESP32-C3 UART Link (existing — unchanged)

| Signal | LuckFox Pin | ESP32-C3 GPIO |
|---|---|---|
| TX (LuckFox → ESP32) | GPIO42 / Pin 1 / `/dev/ttyS2` | GPIO4 (RX) |
| RX (LuckFox ← ESP32) | GPIO43 / Pin 2 / `/dev/ttyS2` | GPIO7 (TX) |
| GND | GND | GND |
| Baud | 921600 | 921600 |

---

## 3. Firmware: `luckfox_audio_fulldup.ino`

**Location in repo:** `luckfox_audio_receiver/luckfox_audio_fulldup.ino`  
**Replaces:** `luckfox_audio_receiver/luckfox_audio_receiver.ino` (speaker-only version)

### 3.1 What Changed vs the Previous Firmware

| Area | Before | After |
|---|---|---|
| I2S channel init | `i2s_new_channel(&cfg, &tx_chan, NULL)` — TX only | `i2s_new_channel(&cfg, &tx_chan, &rx_chan)` — full-duplex |
| INMP441 RX config | Not present | 32-bit slot, Philips standard, DIN=GPIO10 |
| Mic capture | Not present | `capture_and_send_mic()` called every loop |
| 32→16 bit conversion | Not present | `pcm16[i] = (int16_t)(raw32[i] >> 8)` |
| Packet types | 0x01–0x03 (playback only) | Added 0x04 PKT_MIC_START, 0x05 PKT_MIC_DATA, 0x06 PKT_MIC_STOP |
| Mic lifecycle | N/A | Mic starts automatically with PKT_AUDIO_START, stops with PKT_AUDIO_STOP |
| OLED label | "PLAYING" | "PLAY+MIC" when both active |

### 3.2 Full Firmware Source

```cpp
/*
 * Luckfox Audio Full-Duplex for ESP32-C3 AbRobot
 * ─────────────────────────────────────────────────────────────────────────────
 * UART1  : RX=GPIO4 <- Luckfox TX  |  TX=GPIO7 -> Luckfox RX  |  GND <-> GND
 * I2S TX : BCLK=GPIO0, LRC=GPIO1, DOUT=GPIO2  → MAX98357A speaker
 * I2S RX : BCLK=GPIO0, LRC=GPIO1, DIN=GPIO10  ← INMP441 mic   (shared clock)
 * AMP EN : SD/EN=GPIO3  (MAX98357A shutdown control)
 * OLED   : SDA=GPIO5, SCL=GPIO6  (SSD1306 72x40 visible at X=28,Y=12)
 *
 * Packet protocol (UART, both directions):
 *   [0xAA][0x55][TYPE][LEN_LO][LEN_HI][PAYLOAD...][XOR]
 *
 * Downstream (Luckfox → ESP32, playback):
 *   PKT_AUDIO_START 0x01  payload: rate16, bits8, ch8, ...
 *   PKT_AUDIO_DATA  0x02  payload: raw PCM bytes
 *   PKT_AUDIO_STOP  0x03
 *
 * Upstream (ESP32 → Luckfox, capture):
 *   PKT_MIC_START   0x04  sent once when mic is enabled
 *   PKT_MIC_DATA    0x05  payload: raw PCM bytes from INMP441
 *   PKT_MIC_STOP    0x06  sent when mic is disabled
 *   PKT_ACK         0x10
 *   PKT_NACK        0x11
 *   PKT_STATUS      0x12
 * ─────────────────────────────────────────────────────────────────────────────
 * INMP441 wiring:
 *   VDD  → 3.3V    GND → GND
 *   BCLK → GPIO0   WS  → GPIO1   SD → GPIO10
 *   L/R  → GND  (selects LEFT channel / I2S standard left-justified on WS low)
 *
 * MAX98357A wiring (unchanged):
 *   VIN  → 5V      GND → GND
 *   BCLK → GPIO0   LRC → GPIO1   DIN → GPIO2   SD/EN → GPIO3
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "driver/i2s_std.h"
#include <U8g2lib.h>
#include <Wire.h>

#define UART_BAUD    921600
#define UART_RX_PIN  4
#define UART_TX_PIN  7
#define I2S_BCLK     0
#define I2S_LRC      1
#define I2S_DOUT     2
#define SD_PIN       3
#define I2S_DIN      10
#define OLED_SDA     5
#define OLED_SCL     6

#define SYNC_0           0xAA
#define SYNC_1           0x55
#define PKT_AUDIO_START  0x01
#define PKT_AUDIO_DATA   0x02
#define PKT_AUDIO_STOP   0x03
#define PKT_MIC_START    0x04
#define PKT_MIC_DATA     0x05
#define PKT_MIC_STOP     0x06
#define PKT_ACK          0x10
#define PKT_NACK         0x11
#define PKT_STATUS       0x12
#define MAX_PAYLOAD      1024

#define SAMPLE_RATE      16000
#define MIC_CHUNK        512

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);
bool oledReady = false;

void oled_show(const char* line1, const char* line2 = nullptr) {
    if (!oledReady) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(28, 22, line1);
    if (line2) u8g2.drawStr(28, 35, line2);
    u8g2.sendBuffer();
}

#define RING_SIZE    16384
uint8_t ring[RING_SIZE];
volatile uint16_t ring_head = 0, ring_tail = 0;

uint16_t ring_used() { return (ring_head - ring_tail + RING_SIZE) % RING_SIZE; }
uint16_t ring_free() { return RING_SIZE - 1 - ring_used(); }

void ring_write(const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        ring[ring_head] = data[i];
        ring_head = (ring_head + 1) % RING_SIZE;
    }
}

uint16_t ring_read(uint8_t* buf, uint16_t maxlen) {
    uint16_t avail = ring_used();
    uint16_t toread = min(avail, maxlen);
    for (uint16_t i = 0; i < toread; i++) {
        buf[i] = ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return toread;
}

i2s_chan_handle_t tx_chan = NULL;
i2s_chan_handle_t rx_chan = NULL;

bool streaming  = false;
bool mic_active = false;
uint16_t current_sample_rate = SAMPLE_RATE;

void setup_i2s(uint16_t rate) {
    if (tx_chan != NULL) { i2s_channel_disable(tx_chan); i2s_del_channel(tx_chan); tx_chan = NULL; }
    if (rx_chan != NULL) { i2s_channel_disable(rx_chan); i2s_del_channel(rx_chan); rx_chan = NULL; }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan) != ESP_OK) {
        Serial.println("[I2S] channel alloc failed"); return;
    }

    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_BCLK, .ws = (gpio_num_t)I2S_LRC,
            .dout = (gpio_num_t)I2S_DOUT, .din = I2S_GPIO_UNUSED,
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(tx_chan, &tx_cfg) != ESP_OK) {
        Serial.println("[I2S] TX init failed"); return;
    }

    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_BCLK, .ws = (gpio_num_t)I2S_LRC,
            .dout = I2S_GPIO_UNUSED, .din  = (gpio_num_t)I2S_DIN,
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(rx_chan, &rx_cfg) != ESP_OK) {
        Serial.println("[I2S] RX init failed"); return;
    }

    i2s_channel_enable(tx_chan);
    i2s_channel_enable(rx_chan);
    Serial.printf("[I2S] Full-duplex OK @ %d Hz\n", rate);
}

void send_packet(uint8_t type, const uint8_t* payload, uint16_t len) {
    uint8_t header[5] = { SYNC_0, SYNC_1, type,
                          (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF) };
    Serial1.write(header, 5);
    uint8_t xorval = type ^ (len & 0xFF) ^ ((len >> 8) & 0xFF);
    for (uint16_t i = 0; i < len; i++) { Serial1.write(payload[i]); xorval ^= payload[i]; }
    Serial1.write(xorval);
}

void send_response(uint8_t type, uint8_t acked_type, uint8_t status_val) {
    uint8_t body[2] = { acked_type, status_val };
    send_packet(type, body, 2);
}

void handle_packet(uint8_t type, uint8_t* data, uint16_t len) {
    switch (type) {
        case PKT_AUDIO_START:
            if (len >= 4) {
                current_sample_rate = data[0] | (data[1] << 8);
                uint8_t bd = data[2], ch = data[3];
                Serial.printf("[RX] Audio start: %dHz/%d-bit/%d-ch\n", current_sample_rate, bd, ch);
                setup_i2s(current_sample_rate);
                ring_head = ring_tail = 0;
                streaming = true;
                mic_active = true;
                uint8_t mic_info[4] = {
                    (uint8_t)(current_sample_rate & 0xFF), (uint8_t)((current_sample_rate >> 8) & 0xFF),
                    16, 1
                };
                send_packet(PKT_MIC_START, mic_info, 4);
                char buf[20];
                snprintf(buf, sizeof(buf), "%dHz %dbit", current_sample_rate, bd);
                oled_show("PLAY+MIC", buf);
                send_response(PKT_ACK, PKT_AUDIO_START, 0);
            }
            break;
        case PKT_AUDIO_DATA:
            if (streaming && len > 0) {
                if (ring_free() >= len) ring_write(data, len);
                else send_response(PKT_NACK, PKT_AUDIO_DATA, 2);
            }
            break;
        case PKT_AUDIO_STOP:
            streaming = false;
            mic_active = false;
            send_packet(PKT_MIC_STOP, nullptr, 0);
            oled_show("READY", "Waiting...");
            send_response(PKT_ACK, PKT_AUDIO_STOP, 0);
            break;
    }
}

enum ParseState { WAIT_SYNC0, WAIT_SYNC1, READ_TYPE, READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM };
ParseState pstate = WAIT_SYNC0;
uint8_t pkt_type; uint16_t pkt_len, payload_idx;
uint8_t payload[MAX_PAYLOAD]; uint8_t running_xor;

void process_byte(uint8_t b) {
    switch (pstate) {
        case WAIT_SYNC0:   if (b == SYNC_0) pstate = WAIT_SYNC1; break;
        case WAIT_SYNC1:   pstate = (b == SYNC_1) ? READ_TYPE : WAIT_SYNC0; break;
        case READ_TYPE:    pkt_type = b; running_xor = b; pstate = READ_LEN0; break;
        case READ_LEN0:    pkt_len = b; running_xor ^= b; pstate = READ_LEN1; break;
        case READ_LEN1:
            pkt_len |= (b << 8); running_xor ^= b; payload_idx = 0;
            pstate = (pkt_len > 0 && pkt_len <= MAX_PAYLOAD) ? READ_PAYLOAD :
                     (pkt_len == 0) ? READ_CHECKSUM : WAIT_SYNC0;
            break;
        case READ_PAYLOAD:
            payload[payload_idx++] = b; running_xor ^= b;
            if (payload_idx >= pkt_len) pstate = READ_CHECKSUM;
            break;
        case READ_CHECKSUM:
            if (b == running_xor) handle_packet(pkt_type, payload, pkt_len);
            else Serial.println("[UART] Checksum error");
            pstate = WAIT_SYNC0;
            break;
    }
}

void capture_and_send_mic() {
    if (!mic_active || rx_chan == NULL) return;
    static int32_t raw32[MIC_CHUNK / 4];
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_chan, raw32, MIC_CHUNK, &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return;
    uint16_t samples = bytes_read / 4;
    static int16_t pcm16[MIC_CHUNK / 4];
    for (uint16_t i = 0; i < samples; i++) {
        pcm16[i] = (int16_t)(raw32[i] >> 8);
    }
    send_packet(PKT_MIC_DATA, (uint8_t*)pcm16, samples * 2);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    oledReady = true;
    oled_show("LuckFox", "FullDuplex");
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);
    Serial1.setRxBufferSize(4096);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    setup_i2s(SAMPLE_RATE);
    oled_show("READY", "Waiting...");
    Serial.println("[READY]");
}

void loop() {
    while (Serial1.available()) process_byte(Serial1.read());

    if (streaming || ring_used() > 0) {
        uint8_t buf[512];
        uint16_t got = ring_read(buf, sizeof(buf));
        if (got > 0 && tx_chan != NULL) {
            size_t written;
            i2s_channel_write(tx_chan, buf, got, &written, portMAX_DELAY);
        }
    }

    capture_and_send_mic();

    static unsigned long last_status = 0;
    if (streaming && millis() - last_status > 1000) {
        uint8_t fill = (ring_used() * 100) / RING_SIZE;
        send_response(PKT_STATUS, 0, fill);
        char buf[16]; snprintf(buf, sizeof(buf), "Buf:%d%%", fill);
        oled_show("PLAY+MIC", buf);
        last_status = millis();
    }
}
```

### 3.3 INMP441 Technical Notes for Agent

- The INMP441 physically outputs 24-bit audio packed into a 32-bit I2S slot (Philips standard).
- The ESP32-C3 I2S RX must be configured as **32-bit width** to receive it correctly.
- Conversion to 16-bit PCM: `pcm16[i] = (int16_t)(raw32[i] >> 8)` — shifts out the bottom 8 bits, keeping the top 16.
- `L/R` pin tied to GND → mic outputs on the LEFT channel → `I2S_SLOT_MODE_MONO` captures it correctly.
- `i2s_channel_read()` is called with **timeout = 0** (non-blocking) so it never stalls the playback path.

---

## 4. Updated UART Packet Protocol

The protocol is backwards-compatible. The LuckFox can still send only playback packets; the ESP32 will silently not activate the mic. New upstream packets are added:

```
Frame format (unchanged):
  [0xAA] [0x55] [TYPE] [LEN_LO] [LEN_HI] [PAYLOAD × LEN] [XOR]
  XOR = TYPE ^ LEN_LO ^ LEN_HI ^ all_payload_bytes
```

| Type | Hex | Direction | Payload |
|---|---|---|---|
| PKT_AUDIO_START | 0x01 | LuckFox → ESP32 | rate_lo, rate_hi, bits, channels, ... |
| PKT_AUDIO_DATA | 0x02 | LuckFox → ESP32 | raw 16-bit PCM bytes (speaker) |
| PKT_AUDIO_STOP | 0x03 | LuckFox → ESP32 | empty |
| PKT_MIC_START | 0x04 | ESP32 → LuckFox | rate_lo, rate_hi, bits(16), channels(1) |
| PKT_MIC_DATA | 0x05 | ESP32 → LuckFox | raw 16-bit PCM bytes (mic) |
| PKT_MIC_STOP | 0x06 | ESP32 → LuckFox | empty |
| PKT_ACK | 0x10 | ESP32 → LuckFox | acked_type, status(0=OK) |
| PKT_NACK | 0x11 | ESP32 → LuckFox | acked_type, error_code |
| PKT_STATUS | 0x12 | ESP32 → LuckFox | 0x00, ring_fill_percent |

**Mic lifecycle (ESP32-driven):**
1. LuckFox sends `PKT_AUDIO_START` → ESP32 replies `PKT_ACK` + `PKT_MIC_START`
2. ESP32 continuously sends `PKT_MIC_DATA` packets
3. LuckFox sends `PKT_AUDIO_STOP` → ESP32 replies `PKT_ACK` + `PKT_MIC_STOP`

---

## 5. LuckFox Python Changes Required

### 5.1 File to modify: `board/sdcard/audio_sender.py`

This file handles the UART link to the ESP32-C3. It currently only **sends** audio packets. It must be extended to also **receive** mic packets from the ESP32.

Add the following class/function to `audio_sender.py`:

```python
# ── New constants (add alongside existing ones) ────────────────────────────
PKT_MIC_START  = 0x04
PKT_MIC_DATA   = 0x05
PKT_MIC_STOP   = 0x06

# ── Mic receiver class ─────────────────────────────────────────────────────
import threading
import io
import wave

class MicReceiver:
    """
    Runs in a background thread, reads PKT_MIC_DATA packets from the ESP32-C3
    over /dev/ttyS2, and accumulates raw 16-bit PCM into a buffer.
    Call start_recording() before PKT_AUDIO_START is sent.
    Call stop_recording() after PKT_AUDIO_STOP is sent.
    Call get_wav() to retrieve the recorded audio as a WAV bytes object.
    """

    SYNC_0 = 0xAA
    SYNC_1 = 0x55

    def __init__(self, serial_port):
        """
        serial_port: the same serial.Serial instance used by audio_sender
                     (already open at 921600 baud on /dev/ttyS2)
        """
        self._ser = serial_port
        self._pcm_buf = bytearray()
        self._recording = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()
        self._sample_rate = 16000   # updated when PKT_MIC_START received

    def start_recording(self):
        with self._lock:
            self._pcm_buf = bytearray()
            self._recording = True

    def stop_recording(self):
        with self._lock:
            self._recording = False

    def get_wav(self):
        """Returns a bytes object containing a valid WAV file (16-bit, mono)."""
        with self._lock:
            pcm = bytes(self._pcm_buf)
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)          # 16-bit = 2 bytes per sample
            wf.setframerate(self._sample_rate)
            wf.writeframes(pcm)
        return buf.getvalue()

    def _reader_loop(self):
        """Packet parser — mirrors the ESP32 process_byte() state machine."""
        WAIT_SYNC0, WAIT_SYNC1, READ_TYPE, READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM = range(7)
        state = WAIT_SYNC0
        pkt_type = 0
        pkt_len = 0
        payload = bytearray()
        running_xor = 0

        while True:
            try:
                b = self._ser.read(1)
                if not b:
                    continue
                b = b[0]
            except Exception:
                continue

            if state == WAIT_SYNC0:
                if b == self.SYNC_0:
                    state = WAIT_SYNC1

            elif state == WAIT_SYNC1:
                state = READ_TYPE if b == self.SYNC_1 else WAIT_SYNC0

            elif state == READ_TYPE:
                pkt_type = b
                running_xor = b
                state = READ_LEN0

            elif state == READ_LEN0:
                pkt_len = b
                running_xor ^= b
                state = READ_LEN1

            elif state == READ_LEN1:
                pkt_len |= (b << 8)
                running_xor ^= b
                payload = bytearray()
                if pkt_len == 0:
                    state = READ_CHECKSUM
                elif pkt_len <= 1024:
                    state = READ_PAYLOAD
                else:
                    state = WAIT_SYNC0

            elif state == READ_PAYLOAD:
                payload.append(b)
                running_xor ^= b
                if len(payload) >= pkt_len:
                    state = READ_CHECKSUM

            elif state == READ_CHECKSUM:
                if b == running_xor:
                    self._handle_packet(pkt_type, payload)
                state = WAIT_SYNC0

    def _handle_packet(self, ptype, data):
        if ptype == PKT_MIC_START:
            if len(data) >= 2:
                self._sample_rate = data[0] | (data[1] << 8)

        elif ptype == PKT_MIC_DATA:
            with self._lock:
                if self._recording:
                    self._pcm_buf.extend(data)

        elif ptype == PKT_MIC_STOP:
            with self._lock:
                self._recording = False
```

**Important:** The `MicReceiver` thread reads from the **same serial port** as the existing audio sender. This is safe because:
- The existing sender only **writes** to the port (outgoing audio data)
- `MicReceiver` only **reads** from the port (incoming mic data)
- Python's `serial.Serial.read()` is thread-safe for concurrent reads/writes on Linux

### 5.2 File to modify: `board/sdcard/http_api_server_v2.py`

The following changes wire the mic into the agent state machine. Add these changes carefully, preserving all existing logic.

#### Step A — Import and initialise `MicReceiver` at startup

At the top of the file, near where `AudioSender` is initialised:

```python
from audio_sender import AudioSender, MicReceiver   # add MicReceiver to import

# Existing:
audio_sender = AudioSender('/dev/ttyS2', baud=921600)
# New — share the same serial port:
mic_receiver = MicReceiver(audio_sender.ser)        # audio_sender.ser = the serial.Serial instance
```

If `AudioSender` does not expose `.ser` publicly, add this to `AudioSender.__init__` in `audio_sender.py`:

```python
self.ser = serial.Serial(port, baudrate=baud, timeout=0.1)
```

#### Step B — `LISTENING` state: start recording

Find the section in `http_api_server_v2.py` that handles the `CTRL pressed` event (the transition to `LISTENING` state) and add:

```python
def on_ctrl_pressed():
    gui_client.set_state('listening')
    mic_receiver.start_recording()          # ← ADD THIS
    audio_sender.send_audio_start(sample_rate=16000, bits=16, channels=1)
```

#### Step C — `THINKING` state: stop recording and get WAV

Find the `CTRL released` handler (transition to `THINKING` state):

```python
def on_ctrl_released():
    gui_client.set_state('thinking')
    audio_sender.send_audio_stop()          # existing
    mic_receiver.stop_recording()           # ← ADD THIS

    # Retrieve the recorded audio
    wav_bytes = mic_receiver.get_wav()      # ← ADD THIS

    # Forward to MacBook pipeline (existing HTTP call pattern)
    response = send_to_macbook_pipeline(wav_bytes)
    # ... rest of pipeline (LLM → TTS → playback) unchanged
```

#### Step D — Add new API endpoint (optional, for testing)

```python
@app.route('/api/audio/record_status', methods=['GET'])
def record_status():
    return jsonify({
        'recording': mic_receiver._recording,
        'bytes_captured': len(mic_receiver._pcm_buf),
        'sample_rate': mic_receiver._sample_rate
    })
```

---

## 6. Agent State Machine Impact

No changes to the C binary (`luckfox_gui`) are required. The state machine behaviour is unchanged:

```
CTRL pressed  → LISTENING  (mic_receiver.start_recording() called in Python)
CTRL released → THINKING   (mic_receiver.stop_recording() + get_wav() in Python)
LLM done      → SPEAKING   (unchanged)
Playback done → IDLE       (unchanged)
```

The mic recording runs purely in Python/UART — the GUI sees no difference.

---

## 7. Audio Pipeline Data Flow (Complete)

```
[User speaks]
      ↓
[INMP441 mic]
      ↓  I2S DIN GPIO10
[ESP32-C3]  ← full-duplex I2S →  [MAX98357A speaker]
      ↓  UART TX GPIO7 → LuckFox GPIO43 /dev/ttyS2
[PKT_MIC_DATA @ 921600 baud]
      ↓
[MicReceiver thread in audio_sender.py]
      ↓  accumulates raw 16-bit PCM
[mic_receiver.get_wav()]
      ↓  WAV bytes (16kHz, 16-bit, mono)
[http_api_server_v2.py — on_ctrl_released()]
      ↓  HTTP POST to MacBook
[MacBook STT → LLM → TTS]
      ↓  WAV response
[/api/audio/play on LuckFox]
      ↓  audio_sender → PKT_AUDIO_START / PKT_AUDIO_DATA / PKT_AUDIO_STOP
[ESP32-C3 I2S TX → MAX98357A → speaker]
```

---

## 8. Bandwidth and Timing Budget

| Stream | Rate |
|---|---|
| Mic audio (16kHz, 16-bit mono) | 32 KB/s = ~256 kbps |
| Speaker audio (16kHz, 16-bit mono) | 32 KB/s = ~256 kbps |
| Total bidirectional audio | ~512 kbps |
| UART capacity @ 921600 baud | ~92 KB/s = ~736 kbps |
| Margin | ~224 kbps headroom |

The UART link at 921600 baud has sufficient capacity for simultaneous mic + speaker at 16kHz. Do not exceed 16kHz sample rate on this link.

---

## 9. Deployment Steps

### 9.1 Flash the ESP32-C3

1. Open `luckfox_audio_receiver/luckfox_audio_fulldup.ino` in Arduino IDE 2
2. Board settings:
   - Board: `Esp32c3 Dev`
   - CPU Frequency: `160 MHz`
   - Flash Size: `4MB`
   - Flash Mode: `QIO`
   - Upload Speed: `921600`
3. Wire INMP441 SD pin to GPIO10 (only new wire needed)
4. Flash and verify serial monitor shows `[I2S] Full-duplex OK @ 16000 Hz`

### 9.2 Deploy Python changes to LuckFox

```bash
# From MacBook repo root:
./sync.sh push
# or via local network:
./sync.sh --private
```

### 9.3 Verify on board

```bash
ssh root@192.168.1.60

# Confirm ttyS2 is available
ls /dev/ttyS2

# Test mic data is flowing (press CTRL button to trigger LISTENING state)
# then check record status endpoint:
curl http://localhost:8080/api/audio/record_status
# Expected: {"recording": true, "bytes_captured": <increasing>, "sample_rate": 16000}
```

### 9.4 Test end-to-end recording

```python
# Quick test from board shell (python3)
import serial, time, sys
sys.path.insert(0, '/mnt/sdcard')
from audio_sender import MicReceiver, AudioSender

s = serial.Serial('/dev/ttyS2', 921600, timeout=0.1)
mr = MicReceiver(s)
mr.start_recording()
time.sleep(5)   # speak for 5 seconds
mr.stop_recording()
wav = mr.get_wav()
with open('/tmp/test_mic.wav', 'wb') as f:
    f.write(wav)
print(f"Recorded {len(wav)} bytes")
# Copy to MacBook for listening:
# scp root@192.168.1.60:/tmp/test_mic.wav /tmp/test_mic.wav && open /tmp/test_mic.wav
```

---

## 10. Troubleshooting

| Symptom | Likely Cause | Fix |
|---|---|---|
| `[I2S] channel alloc failed` on ESP32 | Previous channel not cleaned up | Reflash; ensure no stale channel handles |
| Mic audio is silent / flat line | L/R pin not tied to GND | Wire INMP441 L/R pin to GND |
| Mic audio is very distorted / clipped | 32→16 bit shift wrong | Confirm `pcm16[i] = (int16_t)(raw32[i] >> 8)` — do NOT use `>> 16` |
| Speaker still works but no PKT_MIC_DATA received | DIN wire missing or wrong GPIO | Confirm GPIO10 ↔ INMP441 SD; check `#define I2S_DIN 10` in firmware |
| `MicReceiver` thread consumes all CPU | `serial.read(1)` in tight loop | Ensure `timeout=0.1` on the Serial object |
| WAV file is empty after recording | `start_recording()` not called before CTRL pressed | Verify call order in `on_ctrl_pressed()` |
| UART parser misses packets | Both threads reading same port | Only `MicReceiver` thread should call `ser.read()` — `AudioSender` only writes |
| GPIO2 (I2S DOUT) causes boot failure | Strapping pin pulled LOW | Check MAX98357A DIN is not driving GPIO2 LOW at boot; add 10kΩ pull-up to 3.3V on GPIO2 if needed |

---

## 11. Files Changed Summary

| File | Change Type | Description |
|---|---|---|
| `luckfox_audio_receiver/luckfox_audio_fulldup.ino` | **New file** | Full-duplex ESP32 firmware (replaces `luckfox_audio_receiver.ino`) |
| `board/sdcard/audio_sender.py` | **Modify** | Add `PKT_MIC_*` constants + `MicReceiver` class |
| `board/sdcard/http_api_server_v2.py` | **Modify** | Init `MicReceiver`, call `start_recording()` on CTRL press, `stop_recording()` + `get_wav()` on CTRL release |
| `CLAUDE.md` | **Update** | Mark audio input blocker as resolved; update section 2 pending list; update section 8 hardware table |

### CLAUDE.md Section 2 Update

Replace the audio input pending item:

```
Before:
1. Audio input hardware — LuckFox Pico Max has NO onboard mic (RV1106 audio ADC pins
   unpopulated). An external mic is required (USB mic or I2S module). Must be resolved
   before audio recording is possible.

After:
1. Audio input hardware — RESOLVED. INMP441 I2S MEMS mic wired to ESP32-C3 GPIO10.
   Full-duplex I2S firmware deployed (luckfox_audio_fulldup.ino). ESP32-C3 streams
   16kHz 16-bit mono PCM upstream via PKT_MIC_DATA over existing UART2 link.
   MicReceiver class in audio_sender.py buffers PCM and exports WAV on demand.
```

### CLAUDE.md Section 8 Hardware Table Update

Add row to the Audio output table (rename section to "Audio — ESP32-C3 via UART2"):

```
| Audio input  | INMP441 I2S mic → ESP32-C3 GPIO10 (DIN) → PKT_MIC_DATA → /dev/ttyS2 |
```
