/*
 * ESP32-C3 Audio Hardware Test v3.1 — with OLED (SH1106 72x40)
 * ─────────────────────────────────────────────────────────────────────────────
 * OLED: SH1106 compatible, 72x40, offset (30,12), I2C on GPIO4(SDA)/GPIO5(SCL)
 * Audio: INMP441 mic + MAX98357A amp, legacy I2S driver
 *
 * Wiring:
 *   I2S BCLK  → GPIO0   (shared by mic + speaker)
 *   I2S WS    → GPIO1   (shared by mic + speaker)
 *   MAX98357A DIN  → GPIO2
 *   MAX98357A SD   → GPIO3  (amp enable)
 *   INMP441 SD     → GPIO10
 *   OLED SDA       → GPIO5
 *   OLED SCL       → GPIO6
 *
 * Commands (Serial 115200):
 *   'r' — init I2S
 *   't' — record 2s + playback (high quality)
 *   'm' — mic test (5s)
 *   'p' — play 440Hz tone (2s)
 *   'f' — full-duplex test (3s)
 *   'd' — dump raw samples
 *   'i' — info
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "driver/i2s.h"
#include <math.h>
#include <string.h>
#include <U8g2lib.h>
#include <Wire.h>

// ── Pin definitions ───────────────────────────────────────────────────────────
#define I2S_BCLK    0
#define I2S_WS      1
#define I2S_DOUT    2
#define SD_PIN      3
#define I2S_DIN     10

#define OLED_SDA    5
#define OLED_SCL    6

// ── Audio config ──────────────────────────────────────────────────────────────
#define SAMPLE_RATE  16000
#define MIC_CHUNK    512

// ── Record buffer (32-bit, 2 seconds) ─────────────────────────────────────────
#define REC_SECONDS  2
#define REC_SAMPLES  (SAMPLE_RATE * REC_SECONDS)
static int32_t rec_buf[REC_SAMPLES];  // 128KB

static bool i2s_ready = false;

// ── OLED ──────────────────────────────────────────────────────────────────────
// SH1106 128x64 but we only use the 72x40 visible area starting at (30,12)
U8G2_SH1106_72X40_WISE_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// ── OLED helper functions ─────────────────────────────────────────────────────
void oled_show(const char* line1, const char* line2 = nullptr, const char* line3 = nullptr) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    if (line1) u8g2.drawStr(0, 10, line1);
    if (line2) u8g2.drawStr(0, 23, line2);
    if (line3) u8g2.drawStr(0, 36, line3);
    u8g2.sendBuffer();
}

void oled_show_progress(const char* title, uint32_t current, uint32_t total) {
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(0, 10, title);

    // Progress bar: 2px from each side, 8px tall
    int barW = 68;
    int barH = 8;
    int barX = 2;
    int barY = 18;
    u8g2.drawFrame(barX, barY, barW, barH);
    int fillW = (int)((float)current / (float)total * (barW - 2));
    if (fillW > barW - 2) fillW = barW - 2;
    if (fillW > 0) u8g2.drawBox(barX + 1, barY + 1, fillW, barH - 2);

    // Time display
    char timeBuf[20];
    float sec = (float)current / (float)SAMPLE_RATE;
    snprintf(timeBuf, sizeof(timeBuf), "%.1fs / %ds", sec, REC_SECONDS);
    u8g2.drawStr(0, 36, timeBuf);

    u8g2.sendBuffer();
}

// ─────────────────────────────────────────────────────────────────────────────
bool setup_i2s(uint32_t rate) {
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_ready = false;

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
        .tx_desc_auto_clear   = true,
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] driver_install FAILED: 0x%x\n", err);
        oled_show("I2S FAIL", "Check wiring");
        return false;
    }

    i2s_pin_config_t pins = {
        .mck_io_num   = I2S_PIN_NO_CHANGE,
        .bck_io_num   = I2S_BCLK,
        .ws_io_num    = I2S_WS,
        .data_out_num = I2S_DOUT,
        .data_in_num  = I2S_DIN,
    };

    err = i2s_set_pin(I2S_NUM_0, &pins);
    if (err != ESP_OK) {
        Serial.printf("[I2S] set_pin FAILED: 0x%x\n", err);
        oled_show("PIN FAIL");
        return false;
    }

    Serial.printf("[I2S] OK — %lu Hz, 32-bit, ONLY_LEFT\n", rate);
    Serial.printf("[I2S] BCLK=GPIO%d  WS=GPIO%d  DOUT=GPIO%d  DIN=GPIO%d\n",
                  I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN);
    Serial.printf("[MEM] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    i2s_ready = true;

    oled_show("I2S OK", "16kHz 32bit", "Send 't'");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
void flush_i2s_rx() {
    int32_t flush[256];
    size_t dummy;
    for (int i = 0; i < 30; i++)
        i2s_read(I2S_NUM_0, flush, sizeof(flush), &dummy, 5 / portTICK_PERIOD_MS);
}

// ─────────────────────────────────────────────────────────────────────────────
// MIC TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_mic() {
    if (!i2s_ready) { Serial.println("[MIC] I2S not ready — run 'r' first"); return; }

    oled_show("MIC TEST", "5 seconds...", "Speak now!");
    Serial.println("\n=== MIC TEST (5 seconds) ===");

    Serial.println("[MIC] Diagnostic read (500ms timeout)...");
    {
        int32_t diag_buf[MIC_CHUNK / 4];
        size_t  diag_bytes = 0;
        esp_err_t diag_err = i2s_read(I2S_NUM_0, diag_buf, MIC_CHUNK, &diag_bytes,
                                       500 / portTICK_PERIOD_MS);
        Serial.printf("[MIC] --> err=0x%x  bytes=%d\n", diag_err, diag_bytes);
    }

    Serial.println("Speak into the INMP441 mic now...");

    static int32_t raw32[MIC_CHUNK / 4];
    uint32_t total_reads   = 0;
    uint32_t success_reads = 0;
    uint32_t total_bytes   = 0;
    int32_t  peak32        = 0;
    int16_t  peak16        = 0;

    uint32_t t_start  = millis();
    uint32_t t_report = t_start + 500;
    uint32_t t_end    = t_start + 5000;

    while (millis() < t_end) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, raw32, MIC_CHUNK, &bytes_read, 100 / portTICK_PERIOD_MS);

        total_reads++;
        if (err == ESP_OK && bytes_read > 0) {
            success_reads++;
            total_bytes += bytes_read;
            for (uint16_t i = 0; i < bytes_read / 4; i++) {
                int32_t s32 = raw32[i]; if (s32 < 0) s32 = -s32;
                if (s32 > peak32) peak32 = s32;
                int16_t s16 = (int16_t)(raw32[i] >> 8);
                int16_t a16 = s16 < 0 ? -s16 : s16;
                if (a16 > peak16) peak16 = a16;
            }
        }

        if (millis() >= t_report) {
            uint32_t elapsed = (millis() - t_start) / 1000;
            char buf[20];
            snprintf(buf, sizeof(buf), "%lus peak:%d", elapsed, peak16);
            oled_show("MIC TEST", buf, "Speak now!");

            Serial.printf("[MIC] t=%lus  reads=%lu  ok=%lu  bytes=%lu  peak32=%ld  peak16=%d\n",
                          elapsed, total_reads, success_reads, total_bytes, peak32, peak16);
            peak32 = 0; peak16 = 0;
            t_report = millis() + 500;
        }
    }

    Serial.println("=== MIC TEST DONE ===");
    Serial.printf("    Total reads : %lu\n", total_reads);
    Serial.printf("    OK reads    : %lu (%.1f%%)\n", success_reads,
                  100.0f * success_reads / (float)(total_reads > 0 ? total_reads : 1));
    Serial.printf("    Total bytes : %lu (%.1f KB/s)\n", total_bytes, total_bytes / 5.0f / 1024.0f);

    if (success_reads == 0) {
        oled_show("MIC FAIL", "No data!", "Check wiring");
    } else if (peak16 < 100) {
        oled_show("MIC LOW", "Very quiet", "Check mic");
    } else {
        oled_show("MIC OK", "Data flowing", "Send 't'");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SPEAKER TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_speaker() {
    if (!i2s_ready) { Serial.println("[SPK] I2S not ready — run 'r' first"); return; }

    oled_show("> TONE", "440Hz 2s", "Listen...");
    Serial.println("\n=== SPEAKER TEST (440Hz tone, 2 seconds) ===");

    const int32_t amp = 20000;
    static int32_t samples[128];
    double phase = 0.0;
    double phase_inc = 2.0 * M_PI * 440.0 / SAMPLE_RATE;
    uint32_t total_written = 0;
    uint32_t t_start = millis();

    while (millis() - t_start < 2000) {
        for (int i = 0; i < 128; i++) {
            samples[i] = ((int32_t)(amp * sin(phase))) << 16;
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        size_t written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, samples, sizeof(samples), &written,
                                   100 / portTICK_PERIOD_MS);
        if (err == ESP_OK) total_written += written;
    }

    Serial.printf("[SPK] Sent %lu bytes\n", total_written);
    Serial.println("=== SPEAKER TEST DONE ===");
    oled_show("> DONE", "Hear tone?", "Send 't'");
}

// ─────────────────────────────────────────────────────────────────────────────
// FULL-DUPLEX TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_fullduplex() {
    if (!i2s_ready) { Serial.println("[FDX] I2S not ready — run 'r' first"); return; }

    oled_show("FULL-DPX", "3 seconds", "Tone + Mic");
    Serial.println("\n=== FULL-DUPLEX TEST (3 seconds) ===");

    static int32_t play_buf[64];
    static int32_t mic_buf[MIC_CHUNK / 4];
    double phase = 0.0;
    double phase_inc = 2.0 * M_PI * 440.0 / SAMPLE_RATE;
    uint32_t total_play = 0, total_mic = 0;
    int16_t  mic_peak = 0;

    uint32_t t_start  = millis();
    uint32_t t_report = t_start + 1000;
    uint32_t t_end    = t_start + 3000;

    while (millis() < t_end) {
        for (int i = 0; i < 64; i++) {
            play_buf[i] = ((int32_t)(16000 * sin(phase))) << 16;
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, play_buf, sizeof(play_buf), &written, 10 / portTICK_PERIOD_MS);
        total_play += written;

        size_t bytes_read = 0;
        if (i2s_read(I2S_NUM_0, mic_buf, MIC_CHUNK, &bytes_read, 10 / portTICK_PERIOD_MS) == ESP_OK
                && bytes_read > 0) {
            total_mic += bytes_read;
            for (uint16_t i = 0; i < bytes_read / 4; i++) {
                int16_t s = (int16_t)(mic_buf[i] >> 8);
                if (s < 0) s = -s;
                if (s > mic_peak) mic_peak = s;
            }
        }

        if (millis() >= t_report) {
            Serial.printf("[FDX] play=%lu  mic=%lu  peak=%d\n", total_play, total_mic, mic_peak);
            mic_peak = 0;
            t_report = millis() + 1000;
        }
    }

    Serial.println("=== FULL-DUPLEX TEST DONE ===");
    if (total_mic == 0)
        oled_show("FDX FAIL", "No mic data");
    else
        oled_show("FDX OK", "Both work!", "Send 't'");
}

// ─────────────────────────────────────────────────────────────────────────────
// RECORD & PLAYBACK (high quality)
// ─────────────────────────────────────────────────────────────────────────────
void test_record_playback() {
    if (!i2s_ready) { Serial.println("[REC] I2S not ready — run 'r' first"); return; }

    Serial.printf("[MEM] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());

    // Flush stale DMA
    flush_i2s_rx();

    // ── Record ──────────────────────────────────────────────────────────────
    oled_show_progress("* REC", 0, REC_SAMPLES);
    Serial.printf("\n[REC] Recording %d samples (%d sec) @ %dHz 32-bit...\n",
                  REC_SAMPLES, REC_SECONDS, SAMPLE_RATE);
    Serial.println("[REC] >>> SPEAK INTO THE MIC NOW! <<<");

    memset(rec_buf, 0, sizeof(rec_buf));
    uint32_t captured = 0;
    uint32_t t_start = millis();
    uint32_t last_oled = 0;

    while (captured < REC_SAMPLES) {
        int32_t tmp[128];
        size_t got = 0;
        esp_err_t err = i2s_read(I2S_NUM_0, tmp, sizeof(tmp), &got, 100 / portTICK_PERIOD_MS);
        if (err == ESP_OK && got > 0) {
            uint16_t n = got / 4;
            for (uint16_t i = 0; i < n && captured < REC_SAMPLES; i++) {
                rec_buf[captured++] = tmp[i];
            }
        }
        // Update OLED ~4 times per second
        uint32_t now = millis();
        if (now - last_oled > 250) {
            oled_show_progress("* REC", captured, REC_SAMPLES);
            last_oled = now;
        }
    }
    Serial.printf("[REC] Captured %lu samples in %lums\n", captured, millis() - t_start);

    // ── Analyze ─────────────────────────────────────────────────────────────
    oled_show("Analyzing...");

    int32_t peak = 0;
    int64_t dc_sum = 0;
    for (uint32_t i = 0; i < REC_SAMPLES; i++) {
        dc_sum += rec_buf[i];
        int32_t v = rec_buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    int32_t dc_offset = (int32_t)(dc_sum / REC_SAMPLES);
    Serial.printf("[REC] Peak raw: %ld  DC offset: %ld\n", peak, dc_offset);

    // Remove DC offset
    for (uint32_t i = 0; i < REC_SAMPLES; i++) {
        rec_buf[i] -= dc_offset;
    }

    // Recalculate peak
    peak = 0;
    for (uint32_t i = 0; i < REC_SAMPLES; i++) {
        int32_t v = rec_buf[i];
        if (v < 0) v = -v;
        if (v > peak) peak = v;
    }
    Serial.printf("[REC] Peak after DC removal: %ld\n", peak);

    // Auto-gain
    float gain = 1.0f;
    if (peak > 0) {
        gain = (float)0x60000000 / (float)peak;
        if (gain > 64.0f) gain = 64.0f;
        if (gain < 0.5f)  gain = 0.5f;
    }
    Serial.printf("[REC] Auto gain: %.2fx\n", gain);

    if (peak < 50000) {
        Serial.println("[REC] WARNING: Very low signal");
    }

    // ── Playback ────────────────────────────────────────────────────────────
    oled_show_progress("> PLAY", 0, REC_SAMPLES);
    Serial.println("[PLAY] Playing back...");
    delay(300);

    uint32_t played = 0;
    last_oled = 0;

    while (played < REC_SAMPLES) {
        int32_t out[128];
        uint16_t n = min((uint32_t)128, (uint32_t)(REC_SAMPLES - played));
        for (uint16_t i = 0; i < n; i++) {
            int64_t amplified = (int64_t)rec_buf[played + i] * (int64_t)(gain * 256.0f);
            amplified >>= 8;
            if (amplified >  0x7FFFFFFFL) amplified =  0x7FFFFFFFL;
            if (amplified < -0x7FFFFFFFL) amplified = -0x7FFFFFFFL;
            out[i] = (int32_t)amplified;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, out, n * 4, &written, 100 / portTICK_PERIOD_MS);
        played += n;

        uint32_t now = millis();
        if (now - last_oled > 250) {
            oled_show_progress("> PLAY", played, REC_SAMPLES);
            last_oled = now;
        }
    }

    // Flush silence
    {
        int32_t silence[128] = {0};
        size_t w;
        for (int i = 0; i < 8; i++)
            i2s_write(I2S_NUM_0, silence, sizeof(silence), &w, 50 / portTICK_PERIOD_MS);
    }

    Serial.println("[PLAY] Done.");
    char gainBuf[20];
    snprintf(gainBuf, sizeof(gainBuf), "Gain: %.1fx", gain);
    oled_show("DONE!", gainBuf, "Send 't'");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(2000);

    // ── OLED init ───────────────────────────────────────────────────────────
    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    u8g2.setContrast(255);  // Max brightness
    u8g2.clearBuffer();
    u8g2.sendBuffer();

    oled_show("Booting...");

    // ── Amp enable ──────────────────────────────────────────────────────────
    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);

    Serial.println("\n\n╔════════════════════════════════════════╗");
    Serial.println("║  ESP32-C3 Audio Test v3.1 + OLED      ║");
    Serial.println("║  SH1106 72x40 | legacy i2s.h          ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("Pins: BCLK=%d WS=%d DOUT=%d SD=%d DIN=%d\n",
                  I2S_BCLK, I2S_WS, I2S_DOUT, SD_PIN, I2S_DIN);
    Serial.printf("OLED: SDA=%d SCL=%d\n", OLED_SDA, OLED_SCL);
    Serial.printf("[MEM] Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
    Serial.printf("[REC] Buffer: %d samples x 4 = %d KB\n", REC_SAMPLES, REC_SAMPLES * 4 / 1024);
    Serial.println();
    Serial.println("Commands: r=init t=rec+play m=mic p=tone f=fdx d=dump i=info");
    Serial.println("READY.");

    oled_show("READY", "Send 'r'", "then 't'");
}

void loop() {
    if (!Serial.available()) return;
    char cmd = Serial.read();
    while (Serial.available()) Serial.read();

    switch (cmd) {
        case 'r':
            Serial.println("[RESET] Initialising I2S...");
            oled_show("Init I2S...");
            if (setup_i2s(SAMPLE_RATE))
                Serial.println("[RESET] Done.");
            break;
        case 'm': test_mic();              break;
        case 'p': test_speaker();          break;
        case 'f': test_fullduplex();       break;
        case 't': test_record_playback();  break;
        case 'd': {
            if (!i2s_ready) { Serial.println("Run 'r' first"); break; }
            oled_show("RAW DUMP", "Speak now!");
            Serial.println("\n=== RAW SAMPLE DUMP ===");
            flush_i2s_rx();
            int32_t buf[16];
            size_t got = 0;
            i2s_read(I2S_NUM_0, buf, sizeof(buf), &got, 500 / portTICK_PERIOD_MS);
            Serial.printf("bytes_read=%d\n", got);
            for (int i = 0; i < (int)(got / 4); i++) {
                int32_t v = buf[i];
                Serial.printf("[%2d] raw32=0x%08X  >>8=%6d  >>11=%6d  >>16=%6d\n",
                    i, (unsigned)v,
                    (int)(int16_t)(v >> 8),
                    (int)(int16_t)(v >> 11),
                    (int)(int16_t)(v >> 16));
            }
            Serial.println("=== END DUMP ===");
            oled_show("DUMP DONE", "Send 't'");
            break;
        }
        case 'i':
            Serial.printf("i2s_ready=%s  SD_PIN(GPIO%d)=%s\n",
                          i2s_ready ? "true" : "false",
                          SD_PIN,
                          digitalRead(SD_PIN) ? "HIGH(amp on)" : "LOW(amp off)");
            Serial.printf("Free heap: %lu bytes\n", (unsigned long)ESP.getFreeHeap());
            {
                char heapBuf[20];
                snprintf(heapBuf, sizeof(heapBuf), "Heap:%luK", (unsigned long)ESP.getFreeHeap() / 1024);
                oled_show("INFO", i2s_ready ? "I2S: OK" : "I2S: OFF", heapBuf);
            }
            break;
        default:
            Serial.println("Commands: r=init t=rec+play m=mic p=tone f=fdx d=dump i=info");
            break;
    }
}