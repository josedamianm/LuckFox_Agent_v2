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
 *   L/R  → GND  (selects LEFT channel / I²S standard left-justified on WS low)
 *
 * MAX98357A wiring (unchanged):
 *   VIN  → 3.3V    GND → GND
 *   BCLK → GPIO0   LRC → GPIO1   DIN → GPIO2   SD/EN → GPIO3
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "driver/i2s_std.h"
#include <U8g2lib.h>
#include <Wire.h>

// ── UART ─────────────────────────────────────────────────────────────────────
#define UART_BAUD    921600
#define UART_RX_PIN  4
#define UART_TX_PIN  7

// ── I2S shared clocks ─────────────────────────────────────────────────────────
#define I2S_BCLK     0
#define I2S_LRC      1

// ── MAX98357A (speaker) ───────────────────────────────────────────────────────
#define I2S_DOUT     2      // ⚠ strapping pin – works if amp doesn't pull LOW at boot
#define SD_PIN       3      // amp shutdown/enable

// ── INMP441 (microphone) ──────────────────────────────────────────────────────
#define I2S_DIN      10     // safe pin, only new wire needed

// ── OLED ─────────────────────────────────────────────────────────────────────
#define OLED_SDA     5
#define OLED_SCL     6

// ── Protocol ─────────────────────────────────────────────────────────────────
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

// ── Audio config ──────────────────────────────────────────────────────────────
#define SAMPLE_RATE      16000
// INMP441 outputs 24-bit left-justified in a 32-bit slot.
// We read 32-bit words and right-shift to get 16-bit PCM.
#define MIC_CHUNK        512    // bytes per mic read iteration

// ── OLED ─────────────────────────────────────────────────────────────────────
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

// ── Playback ring buffer ──────────────────────────────────────────────────────
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

// ── I2S handles ───────────────────────────────────────────────────────────────
i2s_chan_handle_t tx_chan = NULL;   // → MAX98357A speaker
i2s_chan_handle_t rx_chan = NULL;   // ← INMP441 mic

bool streaming    = false;   // playback active
bool mic_active   = false;   // capture active
uint16_t current_sample_rate = SAMPLE_RATE;

// ── I2S setup (full-duplex) ───────────────────────────────────────────────────
void setup_i2s(uint16_t rate) {
    // Tear down existing channels cleanly
    if (tx_chan != NULL) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    if (rx_chan != NULL) {
        i2s_channel_disable(rx_chan);
        i2s_del_channel(rx_chan);
        rx_chan = NULL;
    }

    // One channel config creates BOTH tx and rx on the same peripheral (full-duplex)
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;

    // Pass &tx_chan AND &rx_chan — this is what enables full-duplex
    if (i2s_new_channel(&chan_cfg, &tx_chan, &rx_chan) != ESP_OK) {
        Serial.println("[I2S] channel alloc failed");
        return;
    }

    // ── TX (speaker) config ───────────────────────────────────────────────────
    i2s_std_config_t tx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_LRC,
            .dout = (gpio_num_t)I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,          // RX side handled by rx_chan
            .invert_flags = { false, false, false },
        },
    };
    if (i2s_channel_init_std_mode(tx_chan, &tx_cfg) != ESP_OK) {
        Serial.println("[I2S] TX init failed"); return;
    }

    // ── RX (mic) config ───────────────────────────────────────────────────────
    // INMP441 sends 24-bit audio in a 32-bit slot (Philips/I2S standard).
    // We read as 32-bit and shift later to recover 16-bit samples.
    // L/R=GND → LEFT channel. Override default slot_mask (RIGHT) to LEFT.
    i2s_std_config_t rx_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
                        I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK,    // shared with TX – SDK handles this
            .ws   = (gpio_num_t)I2S_LRC,      // shared with TX
            .dout = I2S_GPIO_UNUSED,
            .din  = (gpio_num_t)I2S_DIN,      // GPIO10 from INMP441 SD pin
            .invert_flags = { false, false, false },
        },
    };
    rx_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;  // INMP441 L/R=GND → LEFT channel
    if (i2s_channel_init_std_mode(rx_chan, &rx_cfg) != ESP_OK) {
        Serial.println("[I2S] RX init failed"); return;
    }

    // Enable both channels
    if (i2s_channel_enable(tx_chan) != ESP_OK) {
        Serial.println("[I2S] TX enable failed"); return;
    }
    if (i2s_channel_enable(rx_chan) != ESP_OK) {
        Serial.println("[I2S] RX enable failed"); return;
    }

    Serial.printf("[I2S] Full-duplex OK @ %d Hz\n", rate);
}

// ── Packet builder / sender ───────────────────────────────────────────────────
void send_packet(uint8_t type, const uint8_t* payload, uint16_t len) {
    // Header: SYNC0 SYNC1 TYPE LEN_LO LEN_HI
    uint8_t header[5] = { SYNC_0, SYNC_1, type,
                          (uint8_t)(len & 0xFF),
                          (uint8_t)((len >> 8) & 0xFF) };
    Serial1.write(header, 5);

    uint8_t xorval = type ^ (len & 0xFF) ^ ((len >> 8) & 0xFF);
    for (uint16_t i = 0; i < len; i++) {
        Serial1.write(payload[i]);
        xorval ^= payload[i];
    }
    Serial1.write(xorval);
}

void send_response(uint8_t type, uint8_t acked_type, uint8_t status_val) {
    uint8_t body[2] = { acked_type, status_val };
    send_packet(type, body, 2);
}

// ── Downstream packet handler (Luckfox → ESP32) ──────────────────────────────
void handle_packet(uint8_t type, uint8_t* data, uint16_t len) {
    switch (type) {
        case PKT_AUDIO_START:
            if (len >= 4) {
                current_sample_rate = data[0] | (data[1] << 8);
                uint8_t bd = data[2], ch = data[3];
                Serial.printf("[RX] Audio start: %dHz / %d-bit / %d-ch\n",
                              current_sample_rate, bd, ch);
                setup_i2s(current_sample_rate);
                ring_head = ring_tail = 0;
                streaming = true;
                mic_active = true;   // start capturing too

                // Announce mic stream to Luckfox
                uint8_t mic_info[4] = {
                    (uint8_t)(current_sample_rate & 0xFF),
                    (uint8_t)((current_sample_rate >> 8) & 0xFF),
                    16, 1   // 16-bit mono
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
            Serial.println("[RX] Audio stop");
            send_packet(PKT_MIC_STOP, nullptr, 0);
            oled_show("READY", "Waiting...");
            send_response(PKT_ACK, PKT_AUDIO_STOP, 0);
            break;
    }
}

// ── UART parser state machine (unchanged logic) ───────────────────────────────
enum ParseState { WAIT_SYNC0, WAIT_SYNC1, READ_TYPE,
                  READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM };
ParseState pstate = WAIT_SYNC0;
uint8_t  pkt_type;
uint16_t pkt_len, payload_idx;
uint8_t  payload[MAX_PAYLOAD];
uint8_t  running_xor;

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
            else Serial.println("[UART] Checksum error – packet dropped");
            pstate = WAIT_SYNC0;
            break;
    }
}

// ── Mic capture & upstream send ───────────────────────────────────────────────
// INMP441 gives 32-bit words; we shift right 8 bits to extract the top 16.
// Result is signed 16-bit PCM ready to send upstream.
void capture_and_send_mic() {
    if (!mic_active || rx_chan == NULL) return;

    // Read 32-bit raw samples from I2S
    static int32_t raw32[MIC_CHUNK / 4];
    size_t bytes_read = 0;
    esp_err_t err = i2s_channel_read(rx_chan, raw32, MIC_CHUNK,
                                     &bytes_read, 0);   // non-blocking (timeout=0)
    if (err != ESP_OK || bytes_read == 0) return;

    uint16_t samples = bytes_read / 4;

    // Convert to 16-bit PCM output buffer
    static int16_t pcm16[MIC_CHUNK / 4];
    for (uint16_t i = 0; i < samples; i++) {
        // INMP441: data is in bits [31:8], shift right 8 and cast to 16-bit
        pcm16[i] = (int16_t)(raw32[i] >> 8);
    }

    uint16_t out_bytes = samples * 2;
    send_packet(PKT_MIC_DATA, (uint8_t*)pcm16, out_bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Luckfox Audio Full-Duplex v1 ===");

    // OLED
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    oledReady = true;
    oled_show("LuckFox", "FullDuplex");
    Serial.println("[OLED] OK");

    // MAX98357A enable
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);
    Serial.println("[AMP] MAX98357A enabled");

    // UART to Luckfox
    Serial1.setRxBufferSize(4096);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[UART1] OK");

    // I2S full-duplex
    setup_i2s(SAMPLE_RATE);

    oled_show("READY", "Waiting...");
    Serial.println("[READY] Waiting for Luckfox stream...");
}

void loop() {
    // ── 1. Parse incoming UART bytes from Luckfox ─────────────────────────────
    while (Serial1.available()) process_byte(Serial1.read());

    // ── 2. Drain playback ring → I2S TX → MAX98357A ───────────────────────────
    if (streaming || ring_used() > 0) {
        uint8_t buf[512];
        uint16_t got = ring_read(buf, sizeof(buf));
        if (got > 0 && tx_chan != NULL) {
            size_t written;
            i2s_channel_write(tx_chan, buf, got, &written, portMAX_DELAY);
        }
    }

    // ── 3. Read mic → send PKT_MIC_DATA upstream to Luckfox ──────────────────
    capture_and_send_mic();

    // ── 4. Periodic status packet ──────────────────────────────────────────────
    static unsigned long last_status = 0;
    if (streaming && millis() - last_status > 1000) {
        uint8_t fill = (ring_used() * 100) / RING_SIZE;
        send_response(PKT_STATUS, 0, fill);
        char buf[16];
        snprintf(buf, sizeof(buf), "Buf:%d%%", fill);
        oled_show("PLAY+MIC", buf);
        last_status = millis();
    }
}
