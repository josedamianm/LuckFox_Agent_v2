/*
 * Luckfox Audio Full-Duplex for ESP32-C3 AbRobot — v2.1
 * ────
 * Based on working audio_hw_test v3.1 (SH1106 72x40, DMA 16x128, flush)
 *
 * UART1  : RX=GPIO4 <- Luckfox TX  |  TX=GPIO7 -> Luckfox RX  |  GND <-> GND
 * I2S TX : BCLK=GPIO0, LRC=GPIO1, DOUT=GPIO2  → MAX98357A speaker
 * I2S RX : BCLK=GPIO0, LRC=GPIO1, DIN=GPIO10  ← INMP441 mic   (shared clock)
 * AMP EN : SD/EN=GPIO3  (MAX98357A shutdown control)
 * OLED   : SDA=GPIO5, SCL=GPIO6  (SH1106 72x40)
 *
 * Packet protocol (UART, both directions):
 *   [0xAA][0x55][TYPE][LEN_LO][LEN_HI][PAYLOAD...][XOR]
 * ────
 */

#include "driver/i2s.h"
#include <U8g2lib.h>
#include <Wire.h>

// ── UART ────
#define UART_BAUD    921600
#define UART_RX_PIN  4
#define UART_TX_PIN  7

// ── I2S shared clocks ────
#define I2S_BCLK     0
#define I2S_LRC      1

// ── MAX98357A (speaker) ────
#define I2S_DOUT     2
#define SD_PIN       3

// ── INMP441 (microphone) ────
#define I2S_DIN      10

// ── OLED ────
#define OLED_SDA     5
#define OLED_SCL     6

// ── Protocol ────
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

// ── Audio config (same as test firmware) ────
#define SAMPLE_RATE      16000
#define MIC_CHUNK        512

// ── OLED (same constructor as test firmware) ────
U8G2_SH1106_72X40_WISE_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);
bool oledReady = false;

void oled_show(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
    if (!oledReady) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    if (line1) u8g2.drawStr(0, 10, line1);
    if (line2) u8g2.drawStr(0, 23, line2);
    if (line3) u8g2.drawStr(0, 36, line3);
    u8g2.sendBuffer();
}

void oled_show_bar(const char* title, const char* subtitle, uint8_t percent) {
    if (!oledReady) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, title);
    int barW = 68, barH = 8, barX = 2, barY = 16;
    u8g2.drawFrame(barX, barY, barW, barH);
    int fillW = (int)((float)percent / 100.0f * (barW - 2));
    if (fillW > barW - 2) fillW = barW - 2;
    if (fillW > 0) u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);
    if (subtitle) u8g2.drawStr(0, 36, subtitle);
    u8g2.sendBuffer();
}

// ── Playback ring buffer (stores 16-bit PCM from Luckfox) ────
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

// ── I2S state ────
static bool i2s_running = false;
static uint16_t i2s_current_rate = 0;

bool streaming    = false;
bool mic_active   = false;
uint16_t current_sample_rate = SAMPLE_RATE;

// ── Mic DC offset (calibrated once at boot) ────
static int32_t mic_dc_offset = 0;

// ── DMA flush (same as test firmware) ────
void flush_i2s_rx() {
    int32_t flush[256];
    size_t dummy;
    for (int i = 0; i < 30; i++)
        i2s_read(I2S_NUM_0, flush, sizeof(flush), &dummy, 5 / portTICK_PERIOD_MS);
}

// ── I2S setup (same DMA config as test firmware: 16x128) ────
// Skips reinstall if already running at the requested rate.
void setup_i2s(uint16_t rate) {
    if (i2s_running && i2s_current_rate == rate) {
        Serial.printf("[I2S] Already running @ %d Hz, skipping\n", rate);
        return;
    }

    if (i2s_running) {
        i2s_driver_uninstall(I2S_NUM_0);
        i2s_running = false;
    }

    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate          = rate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 16,       // ← from test firmware
        .dma_buf_len          = 128,      // ← from test firmware
        .use_apll             = false,
        .tx_desc_auto_clear   = true,
    };

    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[I2S] driver_install failed");
        oled_show("I2S FAIL", "Check wiring");
        return;
    }

    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_LRC,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_DIN,
    };

    if (i2s_set_pin(I2S_NUM_0, &pins) != ESP_OK) {
        Serial.println("[I2S] set_pin failed");
        oled_show("PIN FAIL");
        return;
    }

    i2s_running = true;
    i2s_current_rate = rate;
    Serial.printf("[I2S] Full-duplex OK @ %d Hz (DMA 16x128)\n", rate);
}

// ── Packet builder / sender ────
void send_packet(uint8_t type, const uint8_t* payload, uint16_t len) {
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

// ── Downstream packet handler (Luckfox → ESP32) ────
void handle_packet(uint8_t type, uint8_t* data, uint16_t len) {
    switch (type) {
        case PKT_AUDIO_START:
            if (len >= 4) {
                current_sample_rate = data[0] | (data[1] << 8);
                uint8_t bd = data[2], ch = data[3];
                Serial.printf("[RX] Audio start: %dHz / %d-bit / %d-ch\n",
                    current_sample_rate, bd, ch);

                setup_i2s(current_sample_rate);  // skips if rate unchanged

                // Flush stale DMA before mic starts (same as test firmware)
                flush_i2s_rx();

                ring_head = ring_tail = 0;
                streaming = true;
                mic_active = true;  // starts immediately, no calibration

                uint8_t mic_info[4] = {
                    (uint8_t)(current_sample_rate & 0xFF),
                    (uint8_t)((current_sample_rate >> 8) & 0xFF),
                    16, 1
                };
                send_packet(PKT_MIC_START, mic_info, 4);

                char buf[20];
                snprintf(buf, sizeof(buf), "%dHz %dbit", current_sample_rate, bd);
                oled_show("PLAY+MIC", buf, "Streaming...");
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
            oled_show("STOPPED", "Waiting...");
            send_response(PKT_ACK, PKT_AUDIO_STOP, 0);
            break;
    }
}

// ── UART parser state machine ────
enum ParseState { WAIT_SYNC0, WAIT_SYNC1, READ_TYPE,
                  READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM };
ParseState pstate = WAIT_SYNC0;
uint8_t  pkt_type;
uint16_t pkt_len, payload_idx;
uint8_t  payload_buf[MAX_PAYLOAD];
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
            payload_buf[payload_idx++] = b; running_xor ^= b;
            if (payload_idx >= pkt_len) pstate = READ_CHECKSUM;
            break;
        case READ_CHECKSUM:
            if (b == running_xor) handle_packet(pkt_type, payload_buf, pkt_len);
            else Serial.println("[UART] Checksum error – packet dropped");
            pstate = WAIT_SYNC0;
            break;
    }
}

// ── Mic capture & upstream send ────
void capture_and_send_mic() {
    if (!mic_active || !i2s_running) return;

    static int32_t raw32[MIC_CHUNK / 4];
    static int32_t dc_acc = 0;  // running DC estimate (scaled x256)

    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_NUM_0, raw32, MIC_CHUNK, &bytes_read, 0);
    if (err != ESP_OK || bytes_read == 0) return;

    uint16_t samples = bytes_read / 4;
    static int16_t pcm16[MIC_CHUNK / 4];

    for (uint16_t i = 0; i < samples; i++) {
        int16_t s = (int16_t)(raw32[i] >> 8);
        dc_acc += (int32_t)s - (dc_acc >> 8);
        int16_t dc = (int16_t)(dc_acc >> 8);
        pcm16[i] = s - dc;
    }

    send_packet(PKT_MIC_DATA, (uint8_t*)pcm16, samples * 2);
}

// ── Playback: expand 16-bit PCM to 32-bit for I2S TX ────
void drain_ring_to_speaker() {
    if (!i2s_running) return;
    if (!streaming && ring_used() == 0) return;

    static uint8_t pcm16_buf[512];
    uint16_t got = ring_read(pcm16_buf, sizeof(pcm16_buf));
    got &= ~1u;

    if (got > 0) {
        uint16_t n_samples = got / 2;
        static int32_t pcm32_buf[256];
        int16_t* src = (int16_t*)pcm16_buf;
        for (uint16_t i = 0; i < n_samples; i++) {
            pcm32_buf[i] = (int32_t)src[i] << 16;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, pcm32_buf, n_samples * 4, &written, portMAX_DELAY);
    }
}

// ────
void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Luckfox Audio Full-Duplex v2.1 ===");

    // ── OLED (same init as test firmware) ────
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    u8g2.setContrast(255);
    oledReady = true;
    oled_show("LuckFox", "FullDuplex", "v2.1");
    Serial.println("[OLED] SH1106 72x40 OK");

    // ── Amp enable ────
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);
    Serial.println("[AMP] MAX98357A enabled");

    // ── UART1 to Luckfox ────
    Serial1.setRxBufferSize(4096);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[UART1] OK @ 921600 baud");

    // ── I2S (once at boot) ────
    setup_i2s(SAMPLE_RATE);

    Serial.printf("[MEM] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    oled_show("READY", "Waiting...", "for LuckFox");
    Serial.println("[READY] Waiting for Luckfox stream...");
}

void loop() {
    // ── 1. Parse incoming UART bytes from Luckfox ────
    while (Serial1.available()) process_byte(Serial1.read());

    // ── 2. Drain playback ring → speaker ────
    drain_ring_to_speaker();

    // ── 3. Read mic → send upstream to Luckfox ────
    capture_and_send_mic();

    // ── 4. Periodic status + OLED update ────
    static unsigned long last_status = 0;
    if (millis() - last_status > 1000) {
        if (streaming) {
            uint8_t fill = (ring_used() * 100) / RING_SIZE;
            send_response(PKT_STATUS, 0, fill);
            char buf[20];
            snprintf(buf, sizeof(buf), "Buf:%d%%", fill);
            oled_show_bar("PLAY+MIC", buf, fill);
        }
        last_status = millis();
    }
}