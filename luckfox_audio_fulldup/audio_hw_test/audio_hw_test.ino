/*
 * ESP32-C3 Audio Hardware Test v2.0 — legacy driver/i2s.h
 * ─────────────────────────────────────────────────────────────────────────────
 * Uses the legacy I2S driver (i2s.h) instead of the new i2s_std.h API.
 * The new API has a known full-duplex RX bug on ESP32-C3 (RX DMA never fills).
 *
 * Wiring:
 *   I2S BCLK  → GPIO0   (shared by mic + speaker)
 *   I2S WS    → GPIO1   (shared by mic + speaker)
 *   MAX98357A DIN  → GPIO2
 *   MAX98357A SD   → GPIO3  (amp enable)
 *   INMP441 SD     → GPIO10
 *   INMP441 VDD    → 3.3V    INMP441 GND → GND    INMP441 L/R → GND
 *
 * Commands (Serial Monitor, 115200 baud):
 *   'r' — init I2S
 *   'm' — mic test  (5s capture, reports bytes + peak amplitude)
 *   'p' — play test (2s 440Hz tone through speaker)
 *   'f' — full-duplex test (play tone + capture mic, 3s)
 *   'i' — info
 * ─────────────────────────────────────────────────────────────────────────────
 */

#include "driver/i2s.h"
#include <math.h>
#include <string.h>

// ── Pin definitions ───────────────────────────────────────────────────────────
#define I2S_BCLK    0
#define I2S_WS      1
#define I2S_DOUT    2   // MAX98357A DIN
#define SD_PIN      3   // MAX98357A SD/EN
#define I2S_DIN     10  // INMP441 SD

// ── Audio config ──────────────────────────────────────────────────────────────
#define SAMPLE_RATE  16000
#define MIC_CHUNK    512    // bytes per i2s_read call

static bool i2s_ready = false;

// ─────────────────────────────────────────────────────────────────────────────
bool setup_i2s(uint32_t rate) {
    // Tear down any existing driver
    i2s_driver_uninstall(I2S_NUM_0);
    i2s_ready = false;

    i2s_config_t cfg = {
        .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX),
        .sample_rate          = rate,
        .bits_per_sample      = I2S_BITS_PER_SAMPLE_32BIT,
        .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,  // INMP441 L/R=GND → LEFT
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags     = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count        = 8,
        .dma_buf_len          = 64,
        .use_apll             = false,
        .tx_desc_auto_clear   = true,   // keeps clock running even when TX is idle
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] driver_install FAILED: 0x%x\n", err);
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
        return false;
    }

    Serial.printf("[I2S] OK — %lu Hz, 32-bit, ONLY_LEFT\n", rate);
    Serial.printf("[I2S] BCLK=GPIO%d  WS=GPIO%d  DOUT=GPIO%d  DIN=GPIO%d\n",
                  I2S_BCLK, I2S_WS, I2S_DOUT, I2S_DIN);
    i2s_ready = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// MIC TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_mic() {
    if (!i2s_ready) { Serial.println("[MIC] I2S not ready — run 'r' first"); return; }

    Serial.println("\n=== MIC TEST (5 seconds) ===");

    // Diagnostic: single blocking read with 500ms timeout
    Serial.println("[MIC] Diagnostic read (500ms timeout)...");
    {
        int32_t diag_buf[MIC_CHUNK / 4];
        size_t  diag_bytes = 0;
        esp_err_t diag_err = i2s_read(I2S_NUM_0, diag_buf, MIC_CHUNK, &diag_bytes,
                                       500 / portTICK_PERIOD_MS);
        Serial.printf("[MIC] --> err=0x%x  bytes=%d\n", diag_err, diag_bytes);
        if      (diag_err == ESP_OK && diag_bytes > 0) Serial.println("[MIC] --> DMA OK, data flowing!");
        else if (diag_err == ESP_ERR_TIMEOUT)          Serial.println("[MIC] --> TIMEOUT (hardware issue)");
        else if (diag_err == ESP_OK && diag_bytes == 0) Serial.println("[MIC] --> OK but 0 bytes");
        else    Serial.printf("[MIC] --> UNEXPECTED err=0x%x\n", diag_err);
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
        } else {
            static int ec = 0;
            if (ec++ < 3) {
                Serial.printf("[MIC] read err=0x%x (%s) bytes=%d\n", err,
                    err == ESP_ERR_TIMEOUT ? "TIMEOUT" : "OTHER", bytes_read);
            }
        }

        if (millis() >= t_report) {
            Serial.printf("[MIC] t=%lus  reads=%lu  ok=%lu  bytes=%lu  peak32=%ld  peak16=%d\n",
                          (millis() - t_start) / 1000, total_reads, success_reads,
                          total_bytes, peak32, peak16);
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
        Serial.println("    *** NO DATA FROM MIC ***");
        Serial.println("    INMP441 is not responding — check wiring or replace module.");
    } else if (peak16 < 100) {
        Serial.println("    WARNING: Very low amplitude — mic alive but silent?");
        Serial.println("    Try speaking louder directly into the INMP441.");
    } else {
        Serial.println("    INMP441 OK — producing audio data.");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SPEAKER TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_speaker() {
    if (!i2s_ready) { Serial.println("[SPK] I2S not ready — run 'r' first"); return; }

    Serial.println("\n=== SPEAKER TEST (440Hz tone, 2 seconds) ===");

    const int32_t amp = 20000;
    static int32_t samples[128];
    double phase = 0.0;
    double phase_inc = 2.0 * M_PI * 440.0 / SAMPLE_RATE;
    uint32_t total_written = 0;
    uint32_t t_start = millis();

    while (millis() - t_start < 2000) {
        for (int i = 0; i < 128; i++) {
            // 32-bit frame: place 16-bit value in upper half (bits 31:16)
            samples[i] = ((int32_t)(amp * sin(phase))) << 16;
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        size_t written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0, samples, sizeof(samples), &written,
                                   100 / portTICK_PERIOD_MS);
        if (err == ESP_OK) total_written += written;
        else Serial.printf("[SPK] write err=0x%x\n", err);
    }

    Serial.printf("[SPK] Sent %lu bytes\n", total_written);
    Serial.println("Did you hear 440Hz tone? If not: check MAX98357A wiring.");
    Serial.println("=== SPEAKER TEST DONE ===");
}

// ─────────────────────────────────────────────────────────────────────────────
// FULL-DUPLEX TEST
// ─────────────────────────────────────────────────────────────────────────────
void test_fullduplex() {
    if (!i2s_ready) { Serial.println("[FDX] I2S not ready — run 'r' first"); return; }

    Serial.println("\n=== FULL-DUPLEX TEST (3 seconds) ===");
    Serial.println("Playing 440Hz tone AND capturing mic simultaneously...");

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
        // Write tone to speaker
        for (int i = 0; i < 64; i++) {
            play_buf[i] = ((int32_t)(16000 * sin(phase))) << 16;
            phase += phase_inc;
            if (phase >= 2.0 * M_PI) phase -= 2.0 * M_PI;
        }
        size_t written = 0;
        i2s_write(I2S_NUM_0, play_buf, sizeof(play_buf), &written, 10 / portTICK_PERIOD_MS);
        total_play += written;

        // Read mic (short wait)
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
            Serial.printf("[FDX] play=%lu bytes  mic=%lu bytes  mic_peak=%d\n",
                          total_play, total_mic, mic_peak);
            mic_peak = 0;
            t_report = millis() + 1000;
        }
    }

    Serial.println("=== FULL-DUPLEX TEST DONE ===");
    Serial.printf("    Played  : %lu bytes\n", total_play);
    Serial.printf("    Captured: %lu bytes\n", total_mic);
    if (total_mic == 0)
        Serial.println("    *** MIC captured nothing — INMP441 hardware issue ***");
    else
        Serial.println("    Full-duplex OK!");
}

// ─────────────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    delay(3000);

    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);

    Serial.println("\n\n╔════════════════════════════════════════╗");
    Serial.println("║  ESP32-C3 Audio Hardware Test v2.0    ║");
    Serial.println("║  (legacy driver/i2s.h)                ║");
    Serial.println("╚════════════════════════════════════════╝");
    Serial.printf("Pins: BCLK=%d WS=%d DOUT=%d(spk) SD_EN=%d DIN=%d(mic)\n",
                  I2S_BCLK, I2S_WS, I2S_DOUT, SD_PIN, I2S_DIN);
    Serial.println("[AMP] MAX98357A SD/EN = HIGH");
    Serial.println();
    Serial.println("Send 'r' to init I2S, then test with 'm', 'p', 'f'.");
    Serial.println("Commands: 'r'=init  'm'=mic  'p'=play  'f'=full-duplex  'i'=info");
    Serial.println("READY.");
}

void loop() {
    if (!Serial.available()) return;
    char cmd = Serial.read();
    while (Serial.available()) Serial.read();  // flush

    switch (cmd) {
        case 'r':
            Serial.println("[RESET] Initialising I2S (legacy driver)...");
            if (setup_i2s(SAMPLE_RATE))
                Serial.println("[RESET] Done.");
            break;
        case 'm': test_mic();        break;
        case 'p': test_speaker();    break;
        case 'f': test_fullduplex(); break;
        case 'i':
            Serial.printf("i2s_ready=%s  SD_PIN(GPIO%d)=%s\n",
                          i2s_ready ? "true" : "false",
                          SD_PIN,
                          digitalRead(SD_PIN) ? "HIGH(amp on)" : "LOW(amp off)");
            break;
        default:
            Serial.println("Commands: 'r'=init 'm'=mic 'p'=play 'f'=full-duplex 'i'=info");
            break;
    }
}
