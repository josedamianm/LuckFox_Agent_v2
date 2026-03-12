/*
 * Luckfox Audio Receiver for ESP32-C3 AbRobot
 * UART1: RX=GPIO4 <- Luckfox TX, TX=GPIO7 -> Luckfox RX, GND <-> GND
 * I2S: BCLK=GPIO0, LRC=GPIO1, DOUT=GPIO2, SD/EN=GPIO3
 * OLED: SDA=GPIO5, SCL=GPIO6 (SSD1306 128x64 declared, 72x40 visible at X=28,Y=12)
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
#define OLED_SDA     5
#define OLED_SCL     6

#define SYNC_0           0xAA
#define SYNC_1           0x55
#define PKT_AUDIO_START  0x01
#define PKT_AUDIO_DATA   0x02
#define PKT_AUDIO_STOP   0x03
#define PKT_ACK          0x10
#define PKT_NACK         0x11
#define PKT_STATUS       0x12
#define MAX_PAYLOAD      1024

#define SAMPLE_RATE   16000

U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE, OLED_SCL, OLED_SDA);

bool oledReady = false;

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

bool streaming = false;
uint16_t current_sample_rate = SAMPLE_RATE;
i2s_chan_handle_t tx_chan = NULL;

void oled_show(const char* line1, const char* line2 = nullptr) {
    if (!oledReady) return;
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_6x10_tf);
    u8g2.drawStr(28, 22, line1);
    if (line2) u8g2.drawStr(28, 35, line2);
    u8g2.sendBuffer();
}

void send_response(uint8_t type, uint8_t acked_type, uint8_t status_val) {
    uint8_t pkt[8];
    pkt[0] = SYNC_0; pkt[1] = SYNC_1;
    pkt[2] = type; pkt[3] = 2; pkt[4] = 0;
    pkt[5] = acked_type; pkt[6] = status_val;
    pkt[7] = type ^ 2 ^ 0 ^ acked_type ^ status_val;
    Serial1.write(pkt, 8);
}

void setup_i2s(uint16_t rate) {
    if (tx_chan != NULL) {
        i2s_channel_disable(tx_chan);
        i2s_del_channel(tx_chan);
        tx_chan = NULL;
    }
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    if (i2s_new_channel(&chan_cfg, &tx_chan, NULL) != ESP_OK) {
        Serial.println("I2S channel failed"); return;
    }
    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(rate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = (gpio_num_t)I2S_BCLK,
            .ws   = (gpio_num_t)I2S_LRC,
            .dout = (gpio_num_t)I2S_DOUT,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    if (i2s_channel_init_std_mode(tx_chan, &std_cfg) != ESP_OK) {
        Serial.println("I2S init failed"); return;
    }
    if (i2s_channel_enable(tx_chan) != ESP_OK) {
        Serial.println("I2S enable failed"); return;
    }
    Serial.println("I2S OK");
}

enum ParseState { WAIT_SYNC0, WAIT_SYNC1, READ_TYPE, READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM };
ParseState pstate = WAIT_SYNC0;
uint8_t pkt_type; uint16_t pkt_len, payload_idx;
uint8_t payload[MAX_PAYLOAD]; uint8_t running_xor;

void handle_packet(uint8_t type, uint8_t* data, uint16_t len) {
    switch (type) {
        case PKT_AUDIO_START:
            if (len >= 8) {
                current_sample_rate = data[0] | (data[1] << 8);
                uint8_t bd = data[2], ch = data[3];
                Serial.printf("Audio start: %dHz/%d-bit/%dch\n", current_sample_rate, bd, ch);
                setup_i2s(current_sample_rate);
                ring_head = ring_tail = 0;
                streaming = true;
                char buf[20];
                snprintf(buf, sizeof(buf), "%dHz %dbit", current_sample_rate, bd);
                oled_show("PLAYING", buf);
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
            Serial.println("Audio stop");
            oled_show("READY", "Waiting...");
            send_response(PKT_ACK, PKT_AUDIO_STOP, 0);
            break;
    }
}

void process_byte(uint8_t b) {
    switch (pstate) {
        case WAIT_SYNC0: if (b == SYNC_0) pstate = WAIT_SYNC1; break;
        case WAIT_SYNC1: pstate = (b == SYNC_1) ? READ_TYPE : WAIT_SYNC0; break;
        case READ_TYPE: pkt_type = b; running_xor = b; pstate = READ_LEN0; break;
        case READ_LEN0: pkt_len = b; running_xor ^= b; pstate = READ_LEN1; break;
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
            pstate = WAIT_SYNC0;
            break;
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Luckfox Audio Receiver v5 ===");

    Wire.begin(OLED_SDA, OLED_SCL);
    u8g2.begin();
    oledReady = true;
    oled_show("LuckFox", "Audio RX");
    Serial.println("OLED OK");

    pinMode(SD_PIN, OUTPUT);
    digitalWrite(SD_PIN, HIGH);
    Serial.println("MAX98357A enabled");

    Serial1.setRxBufferSize(4096);
    Serial1.begin(UART_BAUD, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);
    Serial.println("UART1 OK");

    setup_i2s(SAMPLE_RATE);

    oled_show("READY", "Waiting...");
    Serial.println("Ready");
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

    static unsigned long last_status = 0;
    if (streaming && millis() - last_status > 1000) {
        uint8_t fill = (ring_used() * 100) / RING_SIZE;
        send_response(PKT_STATUS, 0, fill);
        char buf[16];
        snprintf(buf, sizeof(buf), "Buf:%d%%", fill);
        oled_show("PLAYING", buf);
        last_status = millis();
    }
}
