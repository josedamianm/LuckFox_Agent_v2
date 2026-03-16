/*
 * disp_driver.c
 * Direct SPI display driver for ST7789 240x240 on LuckFox Pico Max.
 * Uses /dev/spidev0.0 + sysfs GPIO for DC, RST, BL.
 * No kernel framebuffer required.
 */
#include "disp_driver.h"
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <linux/spi/spidev.h>

#define DISP_HOR_RES   240
#define DISP_VER_RES   240
#define SPI_DEV        "/dev/spidev0.0"
#define SPI_HZ         32000000
#define GPIO_DC        73
#define GPIO_RST       51
#define GPIO_BL        72
#define XOFF           80
#define YOFF           0

#define DRAW_BUF_LINES 8
static lv_color_t draw_buf_1[DISP_HOR_RES * DRAW_BUF_LINES];
static lv_color_t draw_buf_2[DISP_HOR_RES * DRAW_BUF_LINES];

static int spi_fd    = -1;
static int gpio_dc   = -1;
static int gpio_rst  = -1;
static int gpio_bl   = -1;

/* ── GPIO sysfs helpers ─────────────────────────────────────────── */
static int gpio_open(int pin, const char *dir) {
    char path[64];
    int fd;

    fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8];
        int n = snprintf(buf, sizeof(buf), "%d", pin);
        write(fd, buf, n);
        close(fd);
    }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, dir, strlen(dir)); close(fd); }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", pin);
    return open(path, O_RDWR);
}

static void gpio_write(int fd, int val) {
    lseek(fd, 0, SEEK_SET);
    write(fd, val ? "1" : "0", 1);
}

/* ── SPI helpers ────────────────────────────────────────────────── */
static void spi_xfer(const uint8_t *data, size_t len) {
    const size_t CHUNK = 4096;
    size_t offset = 0;
    while (offset < len) {
        size_t n = len - offset;
        if (n > CHUNK) n = CHUNK;
        write(spi_fd, data + offset, n);
        offset += n;
    }
}

static void lcd_cmd(uint8_t c) {
    gpio_write(gpio_dc, 0);
    spi_xfer(&c, 1);
}

static void lcd_data(const uint8_t *data, size_t len) {
    gpio_write(gpio_dc, 1);
    spi_xfer(data, len);
}

static void lcd_data_byte(uint8_t b) {
    lcd_data(&b, 1);
}

/* ── ST7789 init sequence ───────────────────────────────────────── */
static void st7789_init(void) {
    /* hardware reset */
    gpio_write(gpio_rst, 1); usleep(50000);
    gpio_write(gpio_rst, 0); usleep(100000);
    gpio_write(gpio_rst, 1); usleep(120000);

    lcd_cmd(0x01); usleep(150000);   /* SWRESET */
    lcd_cmd(0x11); usleep(120000);   /* SLPOUT  */

    lcd_cmd(0x3A); lcd_data_byte(0x55);  /* COLMOD: 16-bit RGB565 */

    /* MADCTL: rotation=270 → 0xA0 */
    lcd_cmd(0x36); lcd_data_byte(0xA0);

    /* Porch setting */
    lcd_cmd(0xB2);
    { uint8_t d[] = {0x0C,0x0C,0x00,0x33,0x33}; lcd_data(d, 5); }

    lcd_cmd(0xB7); lcd_data_byte(0x35);
    lcd_cmd(0xBB); lcd_data_byte(0x19);
    lcd_cmd(0xC0); lcd_data_byte(0x2C);
    lcd_cmd(0xC2); lcd_data_byte(0x01);
    lcd_cmd(0xC3); lcd_data_byte(0x12);
    lcd_cmd(0xC4); lcd_data_byte(0x20);
    lcd_cmd(0xC6); lcd_data_byte(0x0F);
    lcd_cmd(0xD0);
    { uint8_t d[] = {0xA4,0xA1}; lcd_data(d, 2); }

    lcd_cmd(0x21);   /* INVON */
    lcd_cmd(0x13);   /* NORON */
    usleep(10000);
    lcd_cmd(0x29);   /* DISPON */
    usleep(120000);

    /* backlight on */
    gpio_write(gpio_bl, 1);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t ca[] = {
        (uint8_t)((x0+XOFF)>>8), (uint8_t)((x0+XOFF)&0xFF),
        (uint8_t)((x1+XOFF)>>8), (uint8_t)((x1+XOFF)&0xFF)
    };
    uint8_t ra[] = {
        (uint8_t)((y0+YOFF)>>8), (uint8_t)((y0+YOFF)&0xFF),
        (uint8_t)((y1+YOFF)>>8), (uint8_t)((y1+YOFF)&0xFF)
    };
    lcd_cmd(0x2A); lcd_data(ca, 4);
    lcd_cmd(0x2B); lcd_data(ra, 4);
    lcd_cmd(0x2C);
}

/* fill screen with a solid big-endian RGB565 color – diagnostic only */
static void st7789_fill_solid(uint8_t hi, uint8_t lo) {
    set_window(0, 0, DISP_HOR_RES - 1, DISP_VER_RES - 1);
    uint8_t row[DISP_HOR_RES * 2];
    for (int x = 0; x < DISP_HOR_RES; x++) { row[x*2] = hi; row[x*2+1] = lo; }
    gpio_write(gpio_dc, 1);
    for (int y = 0; y < DISP_VER_RES; y++) spi_xfer(row, sizeof(row));
}

/* ── LVGL flush callback ────────────────────────────────────────── */
static void flush_cb(lv_display_t *disp, const lv_area_t *area,
                     uint8_t *px_map) {
    set_window((uint16_t)area->x1, (uint16_t)area->y1,
               (uint16_t)area->x2, (uint16_t)area->y2);

    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    lcd_data(px_map, (size_t)(w * h * 2));

    lv_display_flush_ready(disp);
}

/* ── Public API ─────────────────────────────────────────────────── */
lv_display_t *disp_driver_init(void) {
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    uint32_t speed = SPI_HZ;

    spi_fd   = open(SPI_DEV, O_RDWR);
    gpio_dc  = gpio_open(GPIO_DC,  "out");
    gpio_rst = gpio_open(GPIO_RST, "out");
    gpio_bl  = gpio_open(GPIO_BL,  "out");

    ioctl(spi_fd, SPI_IOC_WR_MODE,          &mode);
    ioctl(spi_fd, SPI_IOC_WR_BITS_PER_WORD, &bits);
    ioctl(spi_fd, SPI_IOC_WR_MAX_SPEED_HZ,  &speed);

    st7789_init();

    /* diagnostic: fill blue (0x001F big-endian).
     * If screen goes blue, init+SPI are working.
     * If screen stays red, the init sequence is not reaching the panel. */
    st7789_fill_solid(0x00, 0x1F);
    usleep(500000);

    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAP);
    lv_display_set_buffers(disp, draw_buf_1, draw_buf_2,
                           sizeof(draw_buf_1),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    return disp;
}

void disp_driver_deinit(void) {
    gpio_write(gpio_bl, 0);
    if (spi_fd >= 0)   { close(spi_fd);   spi_fd  = -1; }
    if (gpio_dc >= 0)  { close(gpio_dc);  gpio_dc  = -1; }
    if (gpio_rst >= 0) { close(gpio_rst); gpio_rst = -1; }
    if (gpio_bl >= 0)  { close(gpio_bl);  gpio_bl  = -1; }
}
