/*
 * Luckfox Audio Full-Duplex for ESP32-C3 AbRobot — v3.0
 * ──────────────────────────────────────────────────────────
 * Hardware:
 *   UART1   RX=GPIO4 ← LuckFox TX  │  TX=GPIO7 → LuckFox RX
 *   I2S TX  BCLK=GPIO0, LRC=GPIO1, DOUT=GPIO2  → MAX98357A speaker
 *   I2S RX  BCLK=GPIO0, LRC=GPIO1, DIN=GPIO10  ← INMP441 mic
 *   SD/EN   GPIO3  (MAX98357A amp enable)
 *   OLED    SDA=GPIO5, SCL=GPIO6  (SH1106 72×40)
 *
 * Packet protocol  [0xAA][0x55][TYPE][LEN_LO][LEN_HI][PAYLOAD…][XOR]
 *   0x01 AUDIO_START  LuckFox→ESP32  AI-pipeline: play TTS at 16 kHz + record mic
 *   0x02 AUDIO_DATA   LuckFox→ESP32  Speaker PCM chunk (s16le)
 *   0x03 AUDIO_STOP   LuckFox→ESP32  End AI-pipeline session
 *   0x04 MIC_START    ESP32→LuckFox  Mic activated; payload [rate_lo rate_hi bits ch]
 *   0x05 MIC_DATA     ESP32→LuckFox  Mic PCM chunk (s16le)
 *   0x06 MIC_STOP     ESP32→LuckFox  Mic deactivated
 *   0x07 CALL_START   LuckFox→ESP32  Live call: 8 kHz, 128 ms ring-buffer cap
 *   0x08 CALL_STOP    LuckFox→ESP32  End live call
 *   0x10 ACK          both dirs
 *   0x11 NACK         both dirs  (e.g. ring buffer full)
 *   0x12 STATUS       both dirs  (heartbeat / buffer fill %)
 *
 * Changes vs v2.1:
 *   - CALL_START / CALL_STOP (8 kHz, 128 ms ring cap for live calls)
 *   - ring_free() limited to CALL_RING_LIMIT in call mode
 *   - flush_i2s_rx(): 8 iterations (was 30) — max 40 ms instead of 150 ms
 *   - i2s_write timeout: 20 ms (was portMAX_DELAY)
 *   - Echo suppression in call mode: −6 dB mic when speaker is active
 *   - OLED rewritten: IDLE / PLAYING / RECORDING / PLAY+REC / CALL
 *     with blinking REC indicator, buffer bar, bidirectional call animation
 */

#include "driver/i2s.h"
#include <U8g2lib.h>
#include <Wire.h>

// ── Pins ──────────────────────────────────────────────────
#define UART_BAUD     921600
#define UART_RX_PIN   4
#define UART_TX_PIN   7
#define I2S_BCLK      0
#define I2S_LRC       1
#define I2S_DOUT      2
#define SD_PIN        3
#define I2S_DIN       10
#define OLED_SDA      5
#define OLED_SCL      6

// ── Protocol ──────────────────────────────────────────────
#define SYNC_0           0xAA
#define SYNC_1           0x55
#define PKT_AUDIO_START  0x01
#define PKT_AUDIO_DATA   0x02
#define PKT_AUDIO_STOP   0x03
#define PKT_MIC_START    0x04
#define PKT_MIC_DATA     0x05
#define PKT_MIC_STOP     0x06
#define PKT_CALL_START   0x07
#define PKT_CALL_STOP    0x08
#define PKT_ACK          0x10
#define PKT_NACK         0x11
#define PKT_STATUS       0x12
#define MAX_PAYLOAD      1024

// ── Audio ──────────────────────────────────────────────────
#define SAMPLE_RATE       16000   // AI-pipeline rate
#define CALL_SAMPLE_RATE  8000    // Live-call rate (saves 50 % UART bandwidth)
// MIC_CHUNK / MIC_CHUNK_CALL: I2S read size in bytes (32-bit samples).
// Both yield 16 ms of audio per capture call.
//   MIC_CHUNK      = 128 samples × 4 B = 512 B → 128 s16le per packet @ 16 kHz = 16 ms
//   MIC_CHUNK_CALL =  64 samples × 4 B = 256 B →  64 s16le per packet @  8 kHz = 16 ms
#define MIC_CHUNK         512
#define MIC_CHUNK_CALL    256
// Ring-buffer sizes:
//   RING_SIZE       = 16 384 B → 512 ms @ 16 kHz  (generous for AI TTS prefill)
//   CALL_RING_LIMIT =  2 048 B → 128 ms @ 8 kHz   (low-latency cap for live call)
#define RING_SIZE         16384
#define CALL_RING_LIMIT   2048

// ── Audio / session state ─────────────────────────────────
static bool     call_mode    = false;  // live-call mode active
static bool     streaming    = false;  // LuckFox sending speaker data
static bool     mic_active   = false;  // ESP32 capturing + streaming mic
static uint16_t current_rate = SAMPLE_RATE;

// ── I2S driver state ──────────────────────────────────────
static bool     i2s_running  = false;
static uint16_t i2s_cur_rate = 0;

// ── OLED (SH1106 72×40, font u8g2_font_6x10_tf: 6 px wide, 10 px tall) ──
U8G2_SH1106_72X40_WISE_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
static bool oled_ready = false;

enum OledState { OLED_IDLE, OLED_PLAYING, OLED_RECORDING, OLED_PLAY_REC, OLED_CALL };
static OledState     oled_state   = OLED_IDLE;
static unsigned long oled_last_ms = 0;
static uint8_t       oled_tick    = 0;    // 0-7, increments every 250 ms
static uint8_t       buf_fill_pct = 0;   // updated each OLED tick; used by bar widget
static unsigned long rec_start_ms = 0;   // set when entering a recording state

// ── Ring buffer ───────────────────────────────────────────
static uint8_t  ring[RING_SIZE];
static uint16_t ring_head = 0, ring_tail = 0;

static uint16_t ring_used() {
    return (ring_head - ring_tail + RING_SIZE) % RING_SIZE;
}

// In call mode cap free space at CALL_RING_LIMIT so the LuckFox cannot fill
// more than 128 ms ahead; this enforces the low-latency budget.
static uint16_t ring_free() {
    uint16_t used = ring_used();
    if (call_mode) {
        return (used >= CALL_RING_LIMIT) ? 0 : (uint16_t)(CALL_RING_LIMIT - used);
    }
    return RING_SIZE - 1 - used;
}

static void ring_write(const uint8_t* data, uint16_t len) {
    for (uint16_t i = 0; i < len; i++) {
        ring[ring_head] = data[i];
        ring_head = (ring_head + 1) % RING_SIZE;
    }
}

static uint16_t ring_read(uint8_t* buf, uint16_t maxlen) {
    uint16_t n = ring_used();
    if (n > maxlen) n = maxlen;
    for (uint16_t i = 0; i < n; i++) {
        buf[i] = ring[ring_tail];
        ring_tail = (ring_tail + 1) % RING_SIZE;
    }
    return n;
}

// ── OLED helpers ──────────────────────────────────────────
// Draw string centered horizontally on the 72 px wide display.
static void draw_cx(const char* s, uint8_t y) {
    uint8_t w = (uint8_t)(strlen(s) * 6);
    uint8_t x = (72 > w) ? (72 - w) / 2 : 0;
    u8g2.drawStr(x, y, s);
}

static void oled_set_state(OledState s) {
    if (s == oled_state) return;
    // Latch rec-timer start when entering a recording state for the first time.
    if ((s == OLED_RECORDING || s == OLED_PLAY_REC) &&
        (oled_state != OLED_RECORDING && oled_state != OLED_PLAY_REC)) {
        rec_start_ms = millis();
    }
    oled_state = s;
}

// Render the current state.  Called every 250 ms from loop().
static void oled_update() {
    if (!oled_ready) return;

    bool blink = (oled_tick & 1) == 0;  // toggles every 250 ms → 500 ms blink period

    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);

    switch (oled_state) {

        // ── IDLE ─ "READY / Waiting..." ──
        case OLED_IDLE:
            draw_cx("READY", 14);
            draw_cx("Waiting...", 30);
            break;

        // ── PLAYING ─ label + buffer fill bar + % ──
        case OLED_PLAYING: {
            draw_cx("PLAY", 10);
            u8g2.drawFrame(2, 13, 68, 8);
            uint8_t fw = (uint8_t)((uint32_t)buf_fill_pct * 66 / 100);
            if (fw) u8g2.drawBox(3, 14, fw, 6);
            char pct[12];
            snprintf(pct, sizeof(pct), "BUF:%d%%", buf_fill_pct);
            draw_cx(pct, 36);
            break;
        }

        // ── RECORDING ─ blinking disc + label + timer ──
        case OLED_RECORDING: {
            if (blink) u8g2.drawDisc(5, 8, 4);
            else       u8g2.drawCircle(5, 8, 4);
            u8g2.drawStr(14, 12, "REC");
            unsigned long secs = (millis() - rec_start_ms) / 1000;
            char timer[8];
            snprintf(timer, sizeof(timer), "%lu:%02lu", secs / 60, secs % 60);
            draw_cx(timer, 30);
            break;
        }

        // ── PLAY + REC ─ play label, blinking dot (REC), buffer bar ──
        case OLED_PLAY_REC: {
            u8g2.drawStr(2, 10, "PLAY");
            // Blinking dot at top-right corner = REC indicator
            if (blink) u8g2.drawDisc(66, 5, 4);
            else       u8g2.drawCircle(66, 5, 4);
            u8g2.drawFrame(2, 13, 68, 8);
            uint8_t fw = (uint8_t)((uint32_t)buf_fill_pct * 66 / 100);
            if (fw) u8g2.drawBox(3, 14, fw, 6);
            char pct[12];
            snprintf(pct, sizeof(pct), "BUF:%d%%", buf_fill_pct);
            draw_cx(pct, 36);
            break;
        }

        // ── CALL ─ "CALL" + animated bidirectional arrows + "LIVE" ──
        case OLED_CALL:
            draw_cx("CALL", 10);
            // Alternates ">> <<" (inward) and "<< >>" (outward) every 500 ms
            // to suggest bidirectional live audio.
            draw_cx(blink ? ">> <<" : "<< >>", 24);
            draw_cx("LIVE", 37);
            break;
    }

    u8g2.sendBuffer();
    oled_tick = (oled_tick + 1) & 7;
}

// ── Packet builder ────────────────────────────────────────
static void send_packet(uint8_t type, const uint8_t* payload, uint16_t len) {
    uint8_t hdr[5] = {
        SYNC_0, SYNC_1, type,
        (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF)
    };
    Serial1.write(hdr, 5);
    uint8_t xorval = type ^ hdr[3] ^ hdr[4];
    for (uint16_t i = 0; i < len; i++) {
        Serial1.write(payload[i]);
        xorval ^= payload[i];
    }
    Serial1.write(xorval);
}

static void send_ack(uint8_t acked_type, uint8_t status = 0) {
    uint8_t b[2] = { acked_type, status };
    send_packet(PKT_ACK, b, 2);
}

static void send_nack(uint8_t acked_type, uint8_t reason) {
    uint8_t b[2] = { acked_type, reason };
    send_packet(PKT_NACK, b, 2);
}

// ── I2S ───────────────────────────────────────────────────

// Flush stale DMA samples after I2S init.
// 8 iterations × 5 ms = 40 ms max (v2.1 used 30 × 5 ms = 150 ms, blocking UART).
static void flush_i2s_rx() {
    int32_t tmp[64];
    size_t dummy;
    for (int i = 0; i < 8; i++)
        i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &dummy, 5 / portTICK_PERIOD_MS);
}

// Install (or reinstall) the I2S driver at the requested sample rate.
// Skips if already running at the same rate.
static void setup_i2s(uint16_t rate) {
    if (i2s_running && i2s_cur_rate == rate) return;
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
        .dma_buf_count        = 16,
        .dma_buf_len          = 128,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,  // auto-zero on TX underrun → clean silence
    };
    if (i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL) != ESP_OK) {
        Serial.println("[I2S] driver_install failed");
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
        return;
    }

    i2s_running  = true;
    i2s_cur_rate = rate;
    Serial.printf("[I2S] %d Hz full-duplex (DMA 16×128)\n", rate);
}

// ── Session management ────────────────────────────────────
static void start_session(uint16_t rate, bool is_call) {
    setup_i2s(rate);
    flush_i2s_rx();
    ring_head = ring_tail = 0;
    call_mode    = is_call;
    streaming    = true;
    mic_active   = true;
    current_rate = rate;

    uint8_t mic_info[4] = {
        (uint8_t)(rate & 0xFF), (uint8_t)((rate >> 8) & 0xFF), 16, 1
    };
    send_packet(PKT_MIC_START, mic_info, 4);
    Serial.printf("[SESSION] %s @ %d Hz\n", is_call ? "CALL" : "AI-pipeline", rate);
}

static void stop_session() {
    streaming  = false;
    mic_active = false;
    call_mode  = false;
    send_packet(PKT_MIC_STOP, nullptr, 0);
    Serial.println("[SESSION] stopped");
}

// ── Packet handler ────────────────────────────────────────
static void handle_packet(uint8_t type, uint8_t* data, uint16_t len) {
    switch (type) {

        case PKT_AUDIO_START:
            if (len >= 4) {
                uint16_t rate = (uint16_t)(data[0] | (data[1] << 8));
                Serial.printf("[RX] AUDIO_START %dHz/%dbit/%dch\n", rate, data[2], data[3]);
                start_session(rate, false);
                send_ack(PKT_AUDIO_START);
            }
            break;

        case PKT_AUDIO_DATA:
            if ((streaming || call_mode) && len > 0) {
                if (ring_free() >= len) ring_write(data, len);
                else                   send_nack(PKT_AUDIO_DATA, 2);  // 2 = ring full
            }
            break;

        case PKT_AUDIO_STOP:
            Serial.println("[RX] AUDIO_STOP");
            stop_session();
            send_ack(PKT_AUDIO_STOP);
            break;

        case PKT_CALL_START: {
            uint16_t rate = (len >= 2)
                ? (uint16_t)(data[0] | (data[1] << 8))
                : CALL_SAMPLE_RATE;
            Serial.printf("[RX] CALL_START %d Hz\n", rate);
            start_session(rate, true);
            send_ack(PKT_CALL_START);
            break;
        }

        case PKT_CALL_STOP:
            Serial.println("[RX] CALL_STOP");
            stop_session();
            send_ack(PKT_CALL_STOP);
            break;
    }
}

// ── UART state machine ────────────────────────────────────
enum ParseState { WAIT_SYNC0, WAIT_SYNC1, READ_TYPE,
                  READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM };
static ParseState pstate       = WAIT_SYNC0;
static uint8_t   pkt_type      = 0;
static uint16_t  pkt_len       = 0;
static uint16_t  payload_idx   = 0;
static uint8_t   payload_buf[MAX_PAYLOAD];
static uint8_t   running_xor   = 0;

static void process_byte(uint8_t b) {
    switch (pstate) {
        case WAIT_SYNC0:  if (b == SYNC_0) pstate = WAIT_SYNC1; break;
        case WAIT_SYNC1:  pstate = (b == SYNC_1) ? READ_TYPE : WAIT_SYNC0; break;
        case READ_TYPE:   pkt_type = b; running_xor = b; pstate = READ_LEN0; break;
        case READ_LEN0:   pkt_len = b; running_xor ^= b; pstate = READ_LEN1; break;
        case READ_LEN1:
            pkt_len |= ((uint16_t)b << 8); running_xor ^= b; payload_idx = 0;
            pstate = (pkt_len == 0)        ? READ_CHECKSUM :
                     (pkt_len <= MAX_PAYLOAD) ? READ_PAYLOAD : WAIT_SYNC0;
            break;
        case READ_PAYLOAD:
            payload_buf[payload_idx++] = b; running_xor ^= b;
            if (payload_idx >= pkt_len) pstate = READ_CHECKSUM;
            break;
        case READ_CHECKSUM:
            if (b == running_xor) handle_packet(pkt_type, payload_buf, pkt_len);
            else                  Serial.println("[UART] checksum error – packet dropped");
            pstate = WAIT_SYNC0;
            break;
    }
}

// ── Mic capture → upstream ────────────────────────────────
static void capture_and_send_mic() {
    if (!mic_active || !i2s_running) return;

    // Statically allocated: sized for the largest chunk (MIC_CHUNK / 4 samples).
    static int32_t raw32[MIC_CHUNK / 4];
    static int16_t pcm16[MIC_CHUNK / 4];
    static int32_t dc_acc = 0;  // IIR DC-removal accumulator (persists between calls)

    uint16_t read_bytes = call_mode ? MIC_CHUNK_CALL : MIC_CHUNK;
    size_t got = 0;
    if (i2s_read(I2S_NUM_0, raw32, read_bytes, &got, 0) != ESP_OK || got == 0) return;

    uint16_t samples = (uint16_t)(got / 4);

    // Echo suppression in call mode: attenuate mic by −6 dB when the speaker
    // ring buffer has data (speaker is outputting audio).
    bool suppress = call_mode && (ring_used() > CALL_RING_LIMIT / 8);

    for (uint16_t i = 0; i < samples; i++) {
        // INMP441 outputs 24-bit audio MSB-justified in a 32-bit I2S slot.
        // >> 11 removes the 8 zero LSBs and shifts 3 more for gain headroom.
        // Result is ~−20 dBFS for normal speech — fits cleanly in int16.
        int32_t s = raw32[i] >> 11;

        // First-order IIR high-pass (DC removal): τ ≈ 256 samples
        dc_acc += s - (dc_acc >> 8);
        s -= (dc_acc >> 8);

        // Echo suppression: −6 dB when speaker is active
        if (suppress) s >>= 1;

        // Clamp to int16 range
        if (s >  32767) s =  32767;
        if (s < -32768) s = -32768;
        pcm16[i] = (int16_t)s;
    }

    send_packet(PKT_MIC_DATA, (const uint8_t*)pcm16, samples * 2);
}

// ── Speaker playback ──────────────────────────────────────
static void drain_ring_to_speaker() {
    if (!i2s_running) return;
    if (!streaming && ring_used() == 0) return;

    static uint8_t pcm16_buf[512];
    uint16_t got = ring_read(pcm16_buf, sizeof(pcm16_buf));
    got &= ~1u;  // ensure even (16-bit aligned)
    if (got == 0) return;

    uint16_t n = got / 2;
    static int32_t pcm32_buf[256];
    const int16_t* src = (const int16_t*)pcm16_buf;
    for (uint16_t i = 0; i < n; i++) pcm32_buf[i] = (int32_t)src[i] << 16;

    size_t written = 0;
    // Short timeout keeps the loop responsive; tx_desc_auto_clear handles underrun silence.
    i2s_write(I2S_NUM_0, pcm32_buf, n * 4, &written, pdMS_TO_TICKS(20));
}

// ── Arduino entry points ──────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println("\n=== Luckfox Audio Full-Duplex v3.0 ===");

    // OLED — show IDLE state immediately
    Wire.begin(OLED_SDA, OLED_SCL);
    if (u8g2.begin()) {
        u8g2.setContrast(200);
        oled_ready = true;
        oled_update();
        Serial.println("[OLED] SH1106 72×40 OK");
    } else {
        Serial.println("[OLED] init failed — continuing without display");
    }

    // MAX98357A amp enable
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);
    Serial.println("[AMP] MAX98357A enabled");

    // UART to LuckFox
    Serial1.setRxBufferSize(4096);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("[UART1] 921600 baud");

    // I2S — initial install at default rate; reinits automatically on rate change
    setup_i2s(SAMPLE_RATE);

    Serial.printf("[MEM] free heap: %lu B\n", (unsigned long)ESP.getFreeHeap());
    Serial.println("[READY] waiting for LuckFox");
}

void loop() {
    // 1 — Parse all available UART bytes from LuckFox
    while (Serial1.available()) process_byte(Serial1.read());

    // 2 — Drain speaker ring buffer → I2S TX → MAX98357A
    drain_ring_to_speaker();

    // 3 — Capture INMP441 → I2S RX → send PKT_MIC_DATA upstream
    capture_and_send_mic();

    // 4 — Determine OLED state and refresh every 250 ms
    {
        OledState desired;
        if      (call_mode)               desired = OLED_CALL;
        else if (streaming && mic_active) desired = OLED_PLAY_REC;
        else if (streaming)               desired = OLED_PLAYING;
        else if (mic_active)              desired = OLED_RECORDING;
        else                              desired = OLED_IDLE;
        oled_set_state(desired);

        unsigned long now = millis();
        if (now - oled_last_ms >= 250) {
            oled_last_ms  = now;
            // Effective ring fill: use CALL_RING_LIMIT in call mode, RING_SIZE otherwise.
            uint16_t eff  = call_mode ? CALL_RING_LIMIT : RING_SIZE;
            buf_fill_pct  = (uint8_t)((uint32_t)ring_used() * 100 / eff);
            if (buf_fill_pct > 100) buf_fill_pct = 100;
            oled_update();
        }
    }

    // 5 — Periodic status packet (fill %) to LuckFox every 1 s
    {
        static unsigned long last_status = 0;
        unsigned long now = millis();
        if (now - last_status >= 1000 && (streaming || call_mode)) {
            last_status = now;
            uint8_t body[2] = { 0, buf_fill_pct };
            send_packet(PKT_STATUS, body, 2);
        }
    }
}
