# LuckFox Agent V2 — LVGL v9 Architecture Document

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Current System Analysis (V1)](#2-current-system-analysis-v1)
3. [V2 System Architecture](#3-v2-system-architecture)
4. [Framebuffer Kernel Configuration](#4-framebuffer-kernel-configuration)
5. [LVGL Application Design (C Binary)](#5-lvgl-application-design-c-binary)
6. [IPC Bridge Protocol](#6-ipc-bridge-protocol)
7. [Input Device Driver (indev)](#7-input-device-driver-indev)
8. [GUI Screen Designs (240x240)](#8-gui-screen-designs-240x240)
9. [Python HTTP Server V2](#9-python-http-server-v2)
10. [V2 Project Directory Structure](#10-v2-project-directory-structure)
11. [Build System & Cross-Compilation](#11-build-system--cross-compilation)
12. [Boot Sequence V2](#12-boot-sequence-v2)
13. [Implementation Roadmap](#13-implementation-roadmap)
14. [Risk Analysis & Mitigations](#14-risk-analysis--mitigations)

---

## 1. Executive Summary

This document defines the complete architecture for upgrading the LuckFox Agent from a Python-only
SPI-rendering system (V1) to a dual-process architecture (V2) powered by LVGL v9. The upgrade
replaces the manual framebuffer manipulation with a professional-grade embedded GUI library while
preserving the existing HTTP API contract and audio subsystem.

**Key architectural decision**: A C-based LVGL binary owns the display via Linux framebuffer
(`/dev/fb0`), while the Python HTTP server communicates with it over a Unix Domain Socket using
a compact JSON-line protocol. This cleanly separates rendering performance from API flexibility.

**Target hardware**:
- LuckFox Pico Max (Rockchip RV1106, ARM Cortex-A7, 256MB DDR3)
- Waveshare Pico LCD 1.3" (ST7789, 240x240, SPI)
- 9 GPIO buttons (A, B, X, Y, D-pad, CTRL)
- ESP32-C3 audio coprocessor via UART

---

## 2. Current System Analysis (V1)

### 2.1 Architecture Overview

```
V1: Single-Process Python Architecture

┌─────────────────────────────────────────────────┐
│               http_api_server.py                │
│  ┌──────────┐  ┌───────────┐  ┌──────────────┐ │
│  │ HTTP API │  │ Main Loop │  │ Eyes/Emoji/  │ │
│  │ :8080    │  │ (render)  │  │ Status/Text  │ │
│  │ (thread) │  │           │  │ Renderers    │ │
│  └────┬─────┘  └─────┬─────┘  └──────┬───────┘ │
│       │              │               │          │
│       └──────┬───────┴───────────────┘          │
│              │                                  │
│  ┌───────────▼──────────┐  ┌─────────────────┐  │
│  │ ST7789 Python Driver │  │ ButtonManager   │  │
│  │ (SPI + GPIO)         │  │ (sysfs GPIO)    │  │
│  └───────────┬──────────┘  └────────┬────────┘  │
└──────────────┼──────────────────────┼───────────┘
               │                      │
          /dev/spidev0.0        /sys/class/gpio/
               │                      │
        ┌──────▼──────┐        ┌──────▼──────┐
        │  ST7789 LCD │        │  9x Buttons │
        │  240x240    │        │  (GPIO)     │
        └─────────────┘        └─────────────┘
```

### 2.2 V1 Key Components

| Component | File | Description |
|-----------|------|-------------|
| Main entry | `board/root/main.py` | Launches http_api_server.py via subprocess |
| API + Renderer | `board/sdcard/http_api_server.py` | 1192-line monolith: HTTP API, SPI display driver, button polling, all renderers |
| Audio | `board/sdcard/audio_sender.py` | UART packet protocol to ESP32-C3 |
| Boot | `board/init.d/S99python` | init.d script calling `/root/main.py` |
| Button pull-ups | `board/init.d/S99button_pullups` | devmem register writes for RV1106 IOC |
| SPI overlay | `board/init.d/S20spi0overlay` | Enables spidev0.0 device tree overlay |

### 2.3 V1 Display Pipeline

```
Python bytearray(240*240*2)  ──►  ST7789.show_striped()  ──►  SPI transfer  ──►  LCD
       RGB565 buffer                  8-row stripes              /dev/spidev0.0
```

- Direct SPI control via `python-periphery` library
- Manual RGB565 pixel manipulation in Python bytearrays
- 8-row stripe transfer to avoid SPI buffer limits
- ~25 FPS for eyes animation, 1 FPS for status screen
- GPIO DC (pin 73), RST (pin 51), BL (pin 72) for display control

### 2.4 V1 Button Mapping

| Button | GPIO | sysfs Pin | Function |
|--------|------|-----------|----------|
| A | GPIO1_D1 | 57 | Mode toggle (STATUS ↔ EYES) |
| B | GPIO2_A5 | 69 | Manual blink |
| X | GPIO2_A1 | 65 | (available) |
| Y | GPIO2_A3 | 67 | (available) |
| UP | GPIO1_C7 | 55 | Gaze up |
| DOWN | GPIO2_A0 | 64 | Gaze down |
| LEFT | GPIO2_A4 | 68 | Gaze left |
| RIGHT | GPIO2_A2 | 66 | Gaze right |
| CTRL | GPIO1_C6 | 54 | (available) |

All buttons are active-LOW with hardware pull-ups configured via IOC registers.

### 2.5 V1 HTTP API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/status` | Current mode, uptime |
| GET | `/api/mode/status` | Switch to status screen |
| GET | `/api/mode/eyes` | Switch to eyes animation |
| GET | `/api/emoji/{name}` | Show named emoji |
| GET | `/api/capture` | JPEG from RTSP camera |
| GET | `/api/audio/stop` | Stop audio playback |
| GET | `/api/audio/tone` | Play 440Hz test tone |
| POST | `/api/text` | Display custom text (JSON body) |
| POST | `/api/image` | Display image (binary body) |
| POST | `/api/gif/frames` | Display GIF animation (JSON body) |
| POST | `/api/audio/play` | Play WAV file (binary body) |
| POST | `/api/audio/tone` | Play custom tone (JSON body) |

### 2.6 V1 Limitations Driving V2

1. **Python rendering is slow** — fill_ellipse, put_pixel in pure Python; ~40ms per eyes frame
2. **No GPU/DMA acceleration** — all pixel work is CPU-bound interpreted code
3. **Monolithic design** — display driver, renderer, API server, button handler all in one file
4. **No widget system** — every UI element is hand-drawn with pixel primitives
5. **No anti-aliasing** — jagged edges on all shapes
6. **Limited animation** — no transitions, no scroll, no smooth opacity changes
7. **SPI contention** — framebuffer cannot be shared; display ownership is exclusive

---

## 3. V2 System Architecture

### 3.1 Dual-Process Design

```
V2: Dual-Process Architecture with IPC Bridge

┌─────────────────────────────┐     ┌──────────────────────────────────────┐
│   Python HTTP Server (V2)   │     │        LVGL C Binary (luckfox_gui)   │
│                             │     │                                      │
│  ┌──────────┐ ┌───────────┐ │     │  ┌──────────┐  ┌─────────────────┐  │
│  │ HTTP API │ │ IPC Client│◄├─UDS─├►│ IPC Server│  │  LVGL v9 Core   │  │
│  │ :8080    │ │ (socket)  │ │     │  │ (socket) │  │  ┌───────────┐  │  │
│  └────┬─────┘ └───────────┘ │     │  └──────────┘  │  │ Screens   │  │  │
│       │                     │     │                 │  │ Widgets   │  │  │
│  ┌────▼─────┐               │     │  ┌──────────┐  │  │ Animations│  │  │
│  │ Audio    │               │     │  │ GPIO indev│  │  │ Themes    │  │  │
│  │ Sender   │               │     │  │ (buttons) │  │  └───────────┘  │  │
│  └──────────┘               │     │  └──────────┘  │                  │  │
└─────────────────────────────┘     │                 │  ┌───────────┐  │  │
                                    │                 │  │ fb driver │  │  │
                                    │                 │  └─────┬─────┘  │  │
                                    │                 └────────┼────────┘  │
                                    └──────────────────────────┼──────────┘
                                                               │
                                                          /dev/fb0
                                                               │
                                                        ┌──────▼──────┐
                                                        │  ST7789 LCD │
                                                        │  240x240    │
                                                        └─────────────┘
```

### 3.2 Process Responsibilities

| Process | Language | Runs As | Responsibilities |
|---------|----------|---------|------------------|
| `luckfox_gui` | C | PID 1 child (init.d) | LVGL rendering, framebuffer output, button input, screen management, animations |
| `http_api_server.py` | Python | Daemon thread from main.py | HTTP API, IPC commands to GUI, audio control, camera capture |

### 3.3 Why This Split?

1. **Performance**: C + LVGL renders at native speed with DMA-capable framebuffer writes
2. **Separation of concerns**: GUI rendering vs API/network/audio are fundamentally different workloads
3. **LVGL's threading model**: LVGL is not thread-safe; it must run in a single-threaded event loop. Isolating it in its own process avoids all synchronization issues
4. **Stability**: A Python crash doesn't kill the GUI; the display keeps showing the last screen
5. **Memory efficiency**: LVGL's draw buffers and widget tree are in C heap, not Python's GC-managed heap

### 3.4 Communication Flow

```
External Client                Python Server              LVGL Binary
     │                              │                          │
     │  GET /api/mode/eyes          │                          │
     │─────────────────────────────►│                          │
     │                              │  {"cmd":"screen",        │
     │                              │   "name":"eyes"}         │
     │                              │─────────────────────────►│
     │                              │                          │ (switches screen)
     │                              │  {"status":"ok"}         │
     │                              │◄─────────────────────────│
     │  200 {"mode":"eyes"}         │                          │
     │◄─────────────────────────────│                          │
     │                              │                          │
     │  POST /api/text              │                          │
     │  {"text":"Hello"}            │                          │
     │─────────────────────────────►│                          │
     │                              │  {"cmd":"text",          │
     │                              │   "text":"Hello",        │
     │                              │   "color":"#FFFFFF"}     │
     │                              │─────────────────────────►│
     │                              │                          │ (updates label)
     │                              │  {"status":"ok"}         │
     │                              │◄─────────────────────────│
     │  200 {"mode":"text"}         │                          │
     │◄─────────────────────────────│                          │
```

---

## 4. Framebuffer Kernel Configuration

### 4.1 Current State

The V1 system uses `/dev/spidev0.0` directly. The ST7789 is driven as a raw SPI device with
manual command/data GPIO toggling. There is **no** `/dev/fb0` device.

### 4.2 Required Kernel Changes

To use LVGL's framebuffer driver, the ST7789 must be exposed as a Linux framebuffer device
via the `fbtft` kernel module.

#### 4.2.1 Device Tree Modification

Create a new device tree overlay `enable_st7789_fb.dts`:

```dts
/dts-v1/;
/plugin/;

/ {
    fragment@0 {
        target = <&spi0>;
        __overlay__ {
            status = "okay";
            #address-cells = <1>;
            #size-cells = <0>;

            /* Disable spidev — cannot coexist with fbtft */
            spidev@0 {
                status = "disabled";
            };

            st7789v@0 {
                compatible = "sitronix,st7789v";
                reg = <0>;
                spi-max-frequency = <32000000>;
                rotation = <270>;
                width = <240>;
                height = <240>;
                fps = <30>;
                dc-gpios   = <&gpio1 RK_PB1 GPIO_ACTIVE_HIGH>;  /* GPIO 73 */
                reset-gpios = <&gpio1 RK_PA3 GPIO_ACTIVE_LOW>;  /* GPIO 51 */
                led-gpios   = <&gpio1 RK_PB0 GPIO_ACTIVE_HIGH>; /* GPIO 72 */
                buswidth = <8>;
                debug = <0>;
            };
        };
    };
};
```

#### 4.2.2 Kernel Config Options

These must be enabled in the RV1106 kernel `.config`:

```
CONFIG_FB=y
CONFIG_FB_TFT=m
CONFIG_FB_TFT_ST7789V=m
CONFIG_FB_TFT_FBTFT=m
CONFIG_STAGING=y
```

#### 4.2.3 Init Script: S20st7789fb (replaces S20spi0overlay)

```sh
#!/bin/sh
case "$1" in
    start)
        modprobe fbtft_device name=st7789v gpios="dc:73,reset:51,led:72" \
            speed=32000000 rotate=270 width=240 height=240 \
            buswidth=8 fps=30
        # Wait for /dev/fb0
        for i in 1 2 3 4 5; do
            [ -e /dev/fb0 ] && break
            sleep 0.5
        done
        # Set console blanking off
        echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null
        ;;
    stop)
        modprobe -r fbtft_device
        ;;
esac
```

#### 4.2.4 Alternative: Direct SPI from LVGL (No Kernel FB)

If kernel recompilation is not feasible, LVGL can drive the ST7789 directly via SPI userspace,
bypassing the framebuffer entirely. This approach:

- Uses `/dev/spidev0.0` from C code (same as V1 Python)
- Implements a custom `lv_display_flush_cb` that sends pixel data over SPI
- Requires the same GPIO DC/RST/BL control
- Does NOT require any kernel changes
- Loses mmap/DMA optimization but is functionally equivalent

**Recommendation**: Start with the direct SPI approach (no kernel changes required), then
migrate to framebuffer once a custom kernel is built. The LVGL flush callback is the only
code that changes between the two approaches.

---

## 5. LVGL Application Design (C Binary)

### 5.1 Application Structure

```
luckfox_gui
├── main.c                  ← Entry point, LVGL init, main loop
├── hal/
│   ├── disp_driver.c       ← Display flush callback (fb0 or SPI)
│   ├── disp_driver.h
│   ├── indev_buttons.c     ← GPIO button input device
│   └── indev_buttons.h
├── ipc/
│   ├── ipc_server.c        ← Unix Domain Socket server
│   ├── ipc_server.h
│   ├── cmd_parser.c        ← JSON command parser
│   └── cmd_parser.h
├── screens/
│   ├── scr_status.c        ← Status screen (date, time, IP)
│   ├── scr_status.h
│   ├── scr_eyes.c          ← Eyes animation screen
│   ├── scr_eyes.h
│   ├── scr_emoji.c         ← Emoji display screen
│   ├── scr_emoji.h
│   ├── scr_text.c          ← Custom text screen
│   ├── scr_text.h
│   ├── scr_image.c         ← Image display screen
│   ├── scr_image.h
│   └── scr_manager.c       ← Screen switching logic
│   └── scr_manager.h
├── anim/
│   ├── eyes_anim.c         ← Eyes gaze/blink animation logic
│   └── eyes_anim.h
├── theme/
│   ├── lf_theme.c          ← Custom dark theme for 240x240
│   └── lf_theme.h
└── lv_conf.h               ← LVGL configuration for RV1106
```

### 5.2 Main Loop

```c
/* main.c — simplified structure */
int main(void) {
    lv_init();

    /* Display driver: either framebuffer or direct SPI */
    lv_display_t *disp = disp_driver_init();

    /* Input device: GPIO buttons as encoder-style navigation */
    lv_indev_t *indev = indev_buttons_init();

    /* Apply custom dark theme */
    lf_theme_init(disp);

    /* Initialize all screens */
    scr_manager_init();
    scr_manager_switch(SCR_STATUS);

    /* Start IPC server (non-blocking, fd-based) */
    ipc_server_init("/tmp/luckfox_gui.sock");

    /* Main event loop */
    while (1) {
        lv_timer_handler();         /* LVGL tick + rendering */
        ipc_server_poll();          /* Check for IPC commands */
        usleep(5000);               /* ~5ms = 200Hz tick rate */
    }

    return 0;
}
```

### 5.3 Display Driver

#### 5.3.1 Framebuffer Mode (`/dev/fb0`)

```c
/* disp_driver.c — framebuffer variant */
#define DISP_HOR_RES 240
#define DISP_VER_RES 240

static int fb_fd;
static uint16_t *fb_map;
static lv_color_t draw_buf[DISP_HOR_RES * 20];  /* 20-line draw buffer */

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    uint16_t *src = (uint16_t *)px_map;

    for (int32_t y = area->y1; y <= area->y2; y++) {
        memcpy(&fb_map[y * DISP_HOR_RES + area->x1], src, w * 2);
        src += w;
    }

    lv_display_flush_ready(disp);
}

lv_display_t *disp_driver_init(void) {
    fb_fd = open("/dev/fb0", O_RDWR);
    fb_map = mmap(NULL, DISP_HOR_RES * DISP_VER_RES * 2,
                  PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);

    lv_display_t *disp = lv_display_create(DISP_HOR_RES, DISP_VER_RES);
    lv_display_set_flush_cb(disp, flush_cb);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp, draw_buf, NULL, sizeof(draw_buf),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    return disp;
}
```

#### 5.3.2 Direct SPI Mode (No Kernel FB)

```c
/* disp_driver.c — SPI variant */
#include <linux/spi/spidev.h>

static int spi_fd;
static int gpio_dc_fd, gpio_rst_fd, gpio_bl_fd;

#define CMD_CASET  0x2A
#define CMD_RASET  0x2B
#define CMD_RAMWR  0x2C
#define XOFF       80
#define YOFF       0

static void spi_write(const uint8_t *data, size_t len) {
    struct spi_ioc_transfer tr = {
        .tx_buf = (unsigned long)data,
        .len = len,
        .speed_hz = 32000000,
    };
    ioctl(spi_fd, SPI_IOC_MESSAGE(1), &tr);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    uint8_t ca[] = {(x0+XOFF)>>8, (x0+XOFF)&0xFF, (x1+XOFF)>>8, (x1+XOFF)&0xFF};
    uint8_t ra[] = {(y0+YOFF)>>8, (y0+YOFF)&0xFF, (y1+YOFF)>>8, (y1+YOFF)&0xFF};

    gpio_write(gpio_dc_fd, 0); spi_write((uint8_t[]){CMD_CASET}, 1);
    gpio_write(gpio_dc_fd, 1); spi_write(ca, 4);
    gpio_write(gpio_dc_fd, 0); spi_write((uint8_t[]){CMD_RASET}, 1);
    gpio_write(gpio_dc_fd, 1); spi_write(ra, 4);
    gpio_write(gpio_dc_fd, 0); spi_write((uint8_t[]){CMD_RAMWR}, 1);
    gpio_write(gpio_dc_fd, 1);
}

static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    set_window(area->x1, area->y1, area->x2, area->y2);

    int32_t w = lv_area_get_width(area);
    int32_t h = lv_area_get_height(area);
    spi_write(px_map, w * h * 2);

    lv_display_flush_ready(disp);
}
```

### 5.4 lv_conf.h Key Settings

```c
/* Optimized for RV1106 + 240x240 ST7789 */
#define LV_COLOR_DEPTH          16        /* RGB565 */
#define LV_DPI_DEF              200       /* Dense for 1.3" screen */

#define LV_MEM_CUSTOM           0
#define LV_MEM_SIZE             (48 * 1024)  /* 48KB for LVGL heap */

#define LV_DRAW_BUF_STRIDE_ALIGN   1
#define LV_DRAW_BUF_ALIGN          4

/* Enable only needed features to minimize binary size */
#define LV_USE_LABEL            1
#define LV_USE_IMG              1
#define LV_USE_ARC              1
#define LV_USE_LINE             1
#define LV_USE_CANVAS           1
#define LV_USE_ANIMIMG          0

/* Fonts — built-in */
#define LV_FONT_MONTSERRAT_12   1
#define LV_FONT_MONTSERRAT_16   1
#define LV_FONT_MONTSERRAT_24   1
#define LV_FONT_MONTSERRAT_32   1
#define LV_FONT_DEFAULT         &lv_font_montserrat_16

/* Performance */
#define LV_USE_OS               LV_OS_NONE
#define LV_USE_DRAW_SW          1
#define LV_USE_DRAW_SW_ASM      LV_DRAW_SW_ASM_NONE

/* File system — for loading images from SD */
#define LV_USE_FS_POSIX         1
#define LV_FS_POSIX_LETTER      'S'
#define LV_FS_POSIX_PATH        "/mnt/sdcard"

/* Tick — provided by gettimeofday */
#define LV_TICK_CUSTOM          1
#define LV_TICK_CUSTOM_INCLUDE  <sys/time.h>
#define LV_TICK_CUSTOM_SYS_TIME_EXPR \
    ({struct timeval tv; gettimeofday(&tv, NULL); tv.tv_sec*1000 + tv.tv_usec/1000;})
```

---

## 6. IPC Bridge Protocol

### 6.1 Transport: Unix Domain Socket (Stream)

- **Socket path**: `/tmp/luckfox_gui.sock`
- **Type**: `SOCK_STREAM` (reliable, ordered)
- **Protocol**: Newline-delimited JSON (JSON Lines)
- **Direction**: Bidirectional (Python sends commands, C sends responses/events)
- **Reconnection**: Python client auto-reconnects with 500ms backoff

**Why Unix Domain Sockets over alternatives:**

| Mechanism | Latency | Complexity | Bidirectional | Chosen? |
|-----------|---------|------------|---------------|---------|
| Unix Domain Socket | ~50μs | Medium | Yes | **Yes** |
| Named Pipe (FIFO) | ~50μs | Low | Need 2 pipes | No |
| Shared Memory + sem | ~1μs | High | Manual sync | No |
| TCP localhost | ~100μs | Medium | Yes | No |
| D-Bus | ~500μs | High | Yes | No |

### 6.2 Message Format

Every message is a single JSON object terminated by `\n`:

```
{"cmd":"...", ...}\n
```

Response:
```
{"status":"ok"|"error", ...}\n
```

### 6.3 Command Reference

#### Screen Switching
```json
{"cmd": "screen", "name": "status"}
{"cmd": "screen", "name": "eyes"}
{"cmd": "screen", "name": "emoji", "emoji": "thumbsup"}
{"cmd": "screen", "name": "text", "text": "Hello World", "color": "#FFFFFF", "scale": 3}
{"cmd": "screen", "name": "image"}
```

#### Data Transfer
```json
{"cmd": "image", "data": "<base64-encoded RGB565 or PNG>", "width": 240, "height": 240}
{"cmd": "gif_start", "frame_count": 10}
{"cmd": "gif_frame", "index": 0, "data": "<base64>", "duration_ms": 100}
```

#### Eyes Control
```json
{"cmd": "eyes_gaze", "zone": 3}
{"cmd": "eyes_blink"}
{"cmd": "eyes_expression", "expr": "happy"}
```

#### State Query
```json
{"cmd": "get_state"}
```
Response:
```json
{"status": "ok", "screen": "eyes", "uptime": 12345, "fps": 28}
```

#### Button Events (C → Python)
```json
{"event": "button", "name": "X", "action": "press"}
{"event": "button", "name": "Y", "action": "press"}
```

Buttons A, D-pad, and B are consumed by the LVGL binary for navigation.
Buttons X, Y, and CTRL are forwarded to Python via IPC for custom actions.

### 6.4 IPC Server Implementation (C side)

```c
/* ipc_server.c — non-blocking UDS server */
#define SOCK_PATH "/tmp/luckfox_gui.sock"
#define BUF_SIZE  4096

static int server_fd = -1;
static int client_fd = -1;
static char recv_buf[BUF_SIZE];
static int recv_len = 0;

void ipc_server_init(const char *path) {
    unlink(path);
    server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);

    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    bind(server_fd, (struct sockaddr *)&addr, sizeof(addr));
    listen(server_fd, 1);  /* single client */
}

void ipc_server_poll(void) {
    /* Accept new client if none connected */
    if (client_fd < 0) {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            fcntl(client_fd, F_SETFL, O_NONBLOCK);
            recv_len = 0;
        }
        return;
    }

    /* Read available data */
    int n = read(client_fd, recv_buf + recv_len, BUF_SIZE - recv_len - 1);
    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(client_fd);
            client_fd = -1;
        }
        return;
    }
    recv_len += n;
    recv_buf[recv_len] = '\0';

    /* Process complete lines */
    char *line_start = recv_buf;
    char *newline;
    while ((newline = strchr(line_start, '\n')) != NULL) {
        *newline = '\0';
        cmd_parser_handle(line_start, client_fd);
        line_start = newline + 1;
    }

    /* Compact buffer */
    int remaining = recv_len - (line_start - recv_buf);
    if (remaining > 0) memmove(recv_buf, line_start, remaining);
    recv_len = remaining;
}
```

### 6.5 IPC Client Implementation (Python side)

```python
import socket, json, threading, time

class GUIClient:
    SOCK_PATH = "/tmp/luckfox_gui.sock"

    def __init__(self, event_callback=None):
        self._sock = None
        self._lock = threading.Lock()
        self._event_cb = event_callback
        self._reader_thread = None
        self._running = True
        self._connect()

    def _connect(self):
        while self._running:
            try:
                self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                self._sock.connect(self.SOCK_PATH)
                self._sock.settimeout(1.0)
                if self._reader_thread is None or not self._reader_thread.is_alive():
                    self._reader_thread = threading.Thread(
                        target=self._read_loop, daemon=True)
                    self._reader_thread.start()
                return
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.5)

    def send_cmd(self, cmd_dict):
        msg = json.dumps(cmd_dict) + "\n"
        with self._lock:
            try:
                self._sock.sendall(msg.encode())
            except (BrokenPipeError, OSError):
                self._connect()
                self._sock.sendall(msg.encode())

    def switch_screen(self, name, **kwargs):
        self.send_cmd({"cmd": "screen", "name": name, **kwargs})

    def _read_loop(self):
        buf = ""
        while self._running:
            try:
                data = self._sock.recv(4096).decode()
                if not data:
                    self._connect()
                    continue
                buf += data
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    msg = json.loads(line)
                    if "event" in msg and self._event_cb:
                        self._event_cb(msg)
            except socket.timeout:
                continue
            except Exception:
                self._connect()
```

---

## 7. Input Device Driver (indev)

### 7.1 LVGL Input Model

LVGL v9 supports these input device types:
- **Pointer** (touchscreen) — not applicable
- **Keypad** — maps to button navigation
- **Encoder** — rotary encoder with push
- **Button** — physical buttons mapped to screen coordinates

For the Waveshare LCD 1.3" with D-pad + action buttons, the **keypad** model is the best fit.

### 7.2 Button-to-LVGL Key Mapping

| Physical Button | LVGL Key | Screen Function |
|----------------|----------|-----------------|
| UP | `LV_KEY_UP` | Navigate up / scroll |
| DOWN | `LV_KEY_DOWN` | Navigate down / scroll |
| LEFT | `LV_KEY_LEFT` | Navigate left / previous |
| RIGHT | `LV_KEY_RIGHT` | Navigate right / next |
| A | `LV_KEY_ENTER` | Select / confirm |
| B | `LV_KEY_ESC` | Back / cancel |
| CTRL | `LV_KEY_HOME` | Return to status screen |
| X | (IPC forward) | Custom action → Python |
| Y | (IPC forward) | Custom action → Python |

### 7.3 GPIO Input Driver

```c
/* indev_buttons.c */
#include "lvgl.h"
#include <fcntl.h>
#include <unistd.h>

typedef struct {
    const char *name;
    int gpio_num;
    uint32_t lv_key;
    int fd;
    int last_val;
    uint32_t last_change_ms;
    bool forward_ipc;
} btn_def_t;

#define DEBOUNCE_MS 80

static btn_def_t buttons[] = {
    {"UP",    55, LV_KEY_UP,    -1, 1, 0, false},
    {"DOWN",  64, LV_KEY_DOWN,  -1, 1, 0, false},
    {"LEFT",  68, LV_KEY_LEFT,  -1, 1, 0, false},
    {"RIGHT", 66, LV_KEY_RIGHT, -1, 1, 0, false},
    {"A",     57, LV_KEY_ENTER, -1, 1, 0, false},
    {"B",     69, LV_KEY_ESC,   -1, 1, 0, false},
    {"CTRL",  54, LV_KEY_HOME,  -1, 1, 0, false},
    {"X",     65, 0,            -1, 1, 0, true},
    {"Y",     67, 0,            -1, 1, 0, true},
};
#define BTN_COUNT (sizeof(buttons) / sizeof(buttons[0]))

static uint32_t pending_key = 0;
static lv_indev_state_t pending_state = LV_INDEV_STATE_RELEASED;

static void export_gpio(int pin) {
    char path[64];
    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        char buf[8]; int n = snprintf(buf, sizeof(buf), "%d", pin);
        write(fd, buf, n);
        close(fd);
    }
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", pin);
    fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, "in", 2); close(fd); }
}

static int read_gpio(int fd) {
    char val;
    lseek(fd, 0, SEEK_SET);
    read(fd, &val, 1);
    return val == '0' ? 0 : 1;
}

static void read_cb(lv_indev_t *indev, lv_indev_data_t *data) {
    uint32_t now = lv_tick_get();

    for (int i = 0; i < BTN_COUNT; i++) {
        int val = read_gpio(buttons[i].fd);
        if (val != buttons[i].last_val &&
            (now - buttons[i].last_change_ms) >= DEBOUNCE_MS) {
            buttons[i].last_val = val;
            buttons[i].last_change_ms = now;

            if (val == 0) {  /* pressed (active LOW) */
                if (buttons[i].forward_ipc) {
                    ipc_server_send_button_event(buttons[i].name);
                } else {
                    pending_key = buttons[i].lv_key;
                    pending_state = LV_INDEV_STATE_PRESSED;
                }
            } else {
                if (!buttons[i].forward_ipc && buttons[i].lv_key == pending_key) {
                    pending_state = LV_INDEV_STATE_RELEASED;
                }
            }
        }
    }

    data->key = pending_key;
    data->state = pending_state;
}

lv_indev_t *indev_buttons_init(void) {
    for (int i = 0; i < BTN_COUNT; i++) {
        export_gpio(buttons[i].gpio_num);
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value",
                 buttons[i].gpio_num);
        buttons[i].fd = open(path, O_RDONLY);
    }

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_read_cb(indev, read_cb);
    return indev;
}
```

---

## 8. GUI Screen Designs (240x240)

### 8.1 Design Principles

- **Dark theme**: Pure black (`#000000`) background — OLED-like on LCD, saves power
- **High contrast**: White and cyan text on black for readability
- **Large touch targets**: Even though buttons, keep elements visually distinct
- **Minimal chrome**: No title bars, no borders — maximize content area
- **240x240 constraint**: Every pixel counts; use Montserrat font at appropriate sizes
- **Smooth transitions**: LVGL screen transitions for professional feel

### 8.2 Screen: Status

```
┌──────────────────────────────┐
│ ● STATUS              12:34  │  ← 12px font, green dot = connected
│──────────────────────────────│
│                              │
│        2026-03-11            │  ← 24px Montserrat, white
│                              │
│         12:34:56             │  ← 32px Montserrat, cyan, bold
│                              │
│──────────────────────────────│
│  IP  192.168.1.60            │  ← 16px, gray label + white value
│  UP  3d 14h 22m              │  ← 16px, gray label + white value
│  CPU 23%  MEM 45%            │  ← 16px, with tiny arc gauges
│──────────────────────────────│
│  [A]Select  [B]Eyes  [C]Menu │  ← 12px footer, button hints
└──────────────────────────────┘
```

LVGL implementation:
- `lv_label` for date, time, IP
- `lv_timer` to update time every second
- `lv_arc` (background-only, no knob) for CPU/MEM gauges
- Screen transition: `LV_SCR_LOAD_ANIM_FADE_IN`

### 8.3 Screen: Eyes

```
┌──────────────────────────────┐
│                              │
│                              │
│     ╭────╮      ╭────╮      │
│    │ ●    │    │ ●    │     │  ← Two ellipses with iris+pupil
│    │      │    │      │     │     Iris follows gaze target
│     ╰────╯      ╰────╯      │     Blink = vertical squash
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

LVGL implementation:
- `lv_canvas` for custom draw (eyes require pixel-level ellipse rendering)
- `lv_anim` for smooth gaze interpolation (replaces Python's ease function)
- `lv_timer` at 40ms (25 FPS) for animation tick
- Blink animation via `lv_anim_t` with custom path callback
- Auto-gaze via random target + `lv_anim` with ease-in-out

### 8.4 Screen: Emoji

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│        [EMOJI ART]           │  ← Pre-rendered PNG from SD card
│        240x200 area          │     or drawn with LVGL shapes
│                              │
│                              │
│                              │
│──────────────────────────────│
│       THUMBS UP              │  ← 16px label, centered
└──────────────────────────────┘
```

LVGL implementation:
- `lv_img` loading PNG from `/mnt/sdcard/emojis/thumbsup.png`
- Fallback: `lv_canvas` with shape primitives (arcs, rects, circles)
- `lv_label` for emoji name

### 8.5 Screen: Text

```
┌──────────────────────────────┐
│                              │
│                              │
│                              │
│      Hello World             │  ← Centered label, auto-wrap
│                              │     Font size from API scale param
│                              │     Color from API color param
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

LVGL implementation:
- `lv_label` with `LV_LABEL_LONG_WRAP` mode
- Dynamic font selection based on scale parameter
- Center alignment via `lv_obj_align(label, LV_ALIGN_CENTER, 0, 0)`

### 8.6 Screen: Image

```
┌──────────────────────────────┐
│                              │
│                              │
│    [Full-screen image]       │  ← 240x240 image decoded and displayed
│    Scaled to fit             │     Supports PNG, JPEG, BMP via LVGL
│                              │
│                              │
│                              │
│                              │
│                              │
└──────────────────────────────┘
```

LVGL implementation:
- Image data received via IPC, written to `/tmp/lvgl_img.bin`
- `lv_img_set_src()` with file path or raw pixel descriptor
- `LV_IMG_CF_TRUE_COLOR` for RGB565 data

### 8.7 Screen: Menu (NEW in V2)

```
┌──────────────────────────────┐
│ ● MENU                      │
│──────────────────────────────│
│                              │
│  ► Status                    │  ← Highlighted item (white bg, black text)
│    Eyes                      │
│    Camera Capture            │
│    Audio Test                │
│    System Info               │
│    Network Config            │
│                              │
│──────────────────────────────│
│  [▲▼]Navigate  [A]Select    │
└──────────────────────────────┘
```

LVGL implementation:
- `lv_list` with `lv_group` for keyboard navigation
- D-pad UP/DOWN scrolls, A selects, B goes back
- CTRL button always returns to this menu from any screen

---

## 9. Python HTTP Server V2

### 9.1 Changes from V1

The V2 HTTP server is **dramatically simplified**:

| Removed | Reason |
|---------|--------|
| ST7789 Python driver | LVGL owns the display |
| All pixel rendering code | LVGL handles all drawing |
| ButtonManager | LVGL's indev reads GPIO directly |
| RGB565 buffer manipulation | Gone |
| FONT5X8, fill_ellipse, etc. | LVGL provides all primitives |
| EyesState animation | Ported to C in eyes_anim.c |

| Kept | Reason |
|------|--------|
| HTTP API endpoints | External contract unchanged |
| Audio sender integration | Independent subsystem |
| Camera capture | Independent subsystem |
| Face tracking zone reader | Forwarded via IPC |

| Added | Reason |
|-------|--------|
| GUIClient (IPC) | Sends commands to LVGL binary |
| Button event handler | Receives X/Y/CTRL events from LVGL |

### 9.2 Simplified Server Structure

```python
#!/usr/bin/env python3
"""LuckFox Agent V2 — HTTP API Server (display delegated to LVGL binary via IPC)"""
import json, threading, time, signal, sys
from http.server import HTTPServer, BaseHTTPRequestHandler
from gui_client import GUIClient

try:
    import audio_sender
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT = 8080
gui = None  # GUIClient instance

class APIHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        path = self.path.rstrip('/')
        if path == '/api/status':
            gui.send_cmd({"cmd": "get_state"})
            # ... return state
        elif path == '/api/mode/status':
            gui.switch_screen("status")
            self._json(200, {'mode': 'status'})
        elif path == '/api/mode/eyes':
            gui.switch_screen("eyes")
            self._json(200, {'mode': 'eyes'})
        # ... remaining endpoints delegate to gui.send_cmd()

    def do_POST(self):
        path = self.path.rstrip('/')
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else b''
        if path == '/api/text':
            data = json.loads(body)
            gui.switch_screen("text", text=data.get('text', ''),
                              color=data.get('color', '#FFFFFF'),
                              scale=int(data.get('scale', 3)))
            self._json(200, {'mode': 'text'})
        elif path == '/api/image':
            # Write to shared tmp file, then notify LVGL
            with open('/tmp/lvgl_img.bin', 'wb') as f:
                f.write(body)
            gui.send_cmd({"cmd": "image", "path": "/tmp/lvgl_img.bin",
                          "size": length})
            self._json(200, {'mode': 'image'})
        # ... audio endpoints unchanged

def on_button_event(msg):
    """Handle button events forwarded from LVGL binary."""
    name = msg.get("name")
    if name == "X":
        pass  # custom action
    elif name == "Y":
        pass  # custom action

def main():
    global gui
    gui = GUIClient(event_callback=on_button_event)

    server = HTTPServer(('0.0.0.0', API_PORT), APIHandler)
    server.serve_forever()
```

---

## 10. V2 Project Directory Structure

```
LuckFox_Agent/
├── ARCHITECTURE_V2_LVGL.md          ← This document
├── README.md
├── CLAUDE.md
├── sync.sh                          ← Updated with V2 file mappings
│
├── board/                           ← Files deployed to the board
│   ├── root/
│   │   ├── main.py                  ← V2: launches both luckfox_gui + python server
│   │   ├── enable_spi0_spidev.dtbo
│   │   ├── enable_spi0_spidev.dts
│   │   ├── enable_st7789_fb.dts     ← NEW: framebuffer device tree overlay
│   │   └── enable_uart2.dts
│   │
│   ├── sdcard/
│   │   ├── http_api_server.py       ← V2: simplified, IPC-based
│   │   ├── gui_client.py            ← NEW: Python IPC client class
│   │   ├── audio_sender.py          ← Unchanged
│   │   ├── emojis/                  ← NEW: Pre-rendered emoji PNGs
│   │   │   ├── thumbsup.png
│   │   │   ├── heart.png
│   │   │   ├── smile.png
│   │   │   └── ...
│   │   └── fonts/                   ← NEW: Custom LVGL font files (optional)
│   │
│   ├── executables/
│   │   ├── get_frame.c              ← Existing camera capture
│   │   └── luckfox_gui              ← NEW: Compiled LVGL binary (ARM)
│   │
│   └── init.d/
│       ├── S20st7789fb              ← NEW: replaces S20spi0overlay (if using fb)
│       ├── S20spi0overlay           ← Kept for direct-SPI mode
│       ├── S21uart2overlay
│       ├── S50rtcinit
│       ├── S60ntpd
│       ├── S98frpc
│       ├── S99button_pullups        ← Unchanged
│       └── S99python                ← V2: starts luckfox_gui first, then python
│
├── lvgl_gui/                        ← NEW: LVGL application source tree
│   ├── CMakeLists.txt               ← Top-level CMake build
│   ├── toolchain-rv1106.cmake       ← Cross-compilation toolchain file
│   ├── lv_conf.h                    ← LVGL configuration
│   │
│   ├── src/
│   │   ├── main.c
│   │   ├── hal/
│   │   │   ├── disp_driver.c
│   │   │   ├── disp_driver.h
│   │   │   ├── indev_buttons.c
│   │   │   └── indev_buttons.h
│   │   ├── ipc/
│   │   │   ├── ipc_server.c
│   │   │   ├── ipc_server.h
│   │   │   ├── cmd_parser.c
│   │   │   └── cmd_parser.h
│   │   ├── screens/
│   │   │   ├── scr_manager.c
│   │   │   ├── scr_manager.h
│   │   │   ├── scr_status.c
│   │   │   ├── scr_status.h
│   │   │   ├── scr_eyes.c
│   │   │   ├── scr_eyes.h
│   │   │   ├── scr_emoji.c
│   │   │   ├── scr_emoji.h
│   │   │   ├── scr_text.c
│   │   │   ├── scr_text.h
│   │   │   ├── scr_image.c
│   │   │   ├── scr_image.h
│   │   │   ├── scr_menu.c
│   │   │   └── scr_menu.h
│   │   ├── anim/
│   │   │   ├── eyes_anim.c
│   │   │   └── eyes_anim.h
│   │   └── theme/
│   │       ├── lf_theme.c
│   │       └── lf_theme.h
│   │
│   └── lib/
│       └── lvgl/                    ← LVGL v9 source (git submodule)
│
├── client/                          ← Desktop/remote client (unchanged)
│
├── luckfox_audio_receiver/          ← ESP32-C3 firmware (unchanged)
│
└── assets/                          ← Design assets, emoji source files
```

---

## 11. Build System & Cross-Compilation

### 11.1 Toolchain

The LuckFox Pico Max uses a Rockchip RV1106 (ARM Cortex-A7). The official SDK provides:

```
Toolchain: arm-rockchip830-linux-uclibcgnueabihf
Prefix:    arm-rockchip830-linux-uclibcgnueabihf-
Path:      <luckfox-sdk>/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/
```

### 11.2 CMake Toolchain File

```cmake
# toolchain-rv1106.cmake
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOLCHAIN_PREFIX arm-rockchip830-linux-uclibcgnueabihf)
set(TOOLCHAIN_PATH "$ENV{LUCKFOX_SDK}/tools/linux/toolchain/${TOOLCHAIN_PREFIX}")

set(CMAKE_C_COMPILER   "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-gcc")
set(CMAKE_CXX_COMPILER "${TOOLCHAIN_PATH}/bin/${TOOLCHAIN_PREFIX}-g++")
set(CMAKE_SYSROOT      "${TOOLCHAIN_PATH}/${TOOLCHAIN_PREFIX}/sysroot")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

set(CMAKE_C_FLAGS "-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2" CACHE STRING "")
```

### 11.3 Top-Level CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.16)
project(luckfox_gui C)

set(CMAKE_C_STANDARD 99)

# LVGL configuration
set(LV_CONF_PATH "${CMAKE_SOURCE_DIR}/lv_conf.h" CACHE STRING "" FORCE)
add_compile_definitions(LV_CONF_INCLUDE_SIMPLE)

# Add LVGL library
add_subdirectory(lib/lvgl)

# Application sources
file(GLOB_RECURSE APP_SOURCES
    "src/*.c"
)

add_executable(luckfox_gui ${APP_SOURCES})

target_include_directories(luckfox_gui PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/lib/lvgl
)

target_link_libraries(luckfox_gui PRIVATE
    lvgl
    pthread
    m
)

# Compact binary
target_link_options(luckfox_gui PRIVATE -s)  # strip symbols

install(TARGETS luckfox_gui DESTINATION /root/Executables)
```

### 11.4 Build Commands

```bash
# One-time setup
cd lvgl_gui
git submodule add https://github.com/lvgl/lvgl.git lib/lvgl
cd lib/lvgl && git checkout v9.2 && cd ../..

# Configure
export LUCKFOX_SDK=/path/to/luckfox-pico-sdk
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake

# Build
make -j$(nproc)

# Result: build/luckfox_gui (ARM binary, ~200-400KB stripped)

# Deploy
cp build/luckfox_gui ../board/executables/luckfox_gui
```

### 11.5 Native Build (for development/testing on host)

```bash
mkdir -p build-host && cd build-host
cmake .. -DCMAKE_C_FLAGS="-DSW_SIMULATOR=1"
make -j$(nproc)
# Runs with SDL2 display driver on desktop for UI prototyping
```

---

## 12. Boot Sequence V2

### 12.1 Init Script Order

```
S20spi0overlay  OR  S20st7789fb    ← Enable SPI device OR framebuffer
S21uart2overlay                     ← Enable UART2 for audio
S50rtcinit                          ← RTC initialization
S60ntpd                             ← NTP time sync
S98frpc                             ← FRP tunnel
S99button_pullups                   ← GPIO pull-up registers
S99python                           ← V2: Launch luckfox_gui, then main.py
```

### 12.2 Updated S99python

```sh
#!/bin/sh
GUI_BIN="/root/Executables/luckfox_gui"
MAIN_PY="/root/main.py"

start() {
    # Start LVGL GUI binary first (must own display)
    if [ -x "$GUI_BIN" ]; then
        echo "Starting luckfox_gui..."
        $GUI_BIN &
        sleep 1  # Wait for IPC socket to be ready
    fi

    # Then start Python server
    if [ -f "$MAIN_PY" ]; then
        echo "Starting $MAIN_PY..."
        python $MAIN_PY &
    fi
    echo "OK"
}

stop() {
    killall luckfox_gui 2>/dev/null
    killall python 2>/dev/null
}

case "$1" in
    start)   start ;;
    stop)    stop ;;
    restart) stop; sleep 1; start ;;
    *)       echo "Usage: $0 {start|stop|restart}" ;;
esac
```

### 12.3 Updated main.py

```python
import subprocess, time

time.sleep(1)  # Wait for luckfox_gui IPC socket

subprocess.Popen(
    ["python3", "/mnt/sdcard/http_api_server.py"],
    stdout=open("/tmp/http_api_server.log", "w"),
    stderr=subprocess.STDOUT
)
```

---

## 13. Implementation Roadmap

### Phase 0: Environment Setup
- Install LuckFox Pico SDK and verify cross-compilation toolchain
- Set up LVGL v9 as git submodule
- Create CMake build system with toolchain file
- Verify native (host) build with SDL2 simulator
- Create the `lvgl_gui/` directory structure

### Phase 1: Display Driver (Minimal Viable Display)
- Implement `disp_driver.c` with direct SPI mode (no kernel changes)
- Port ST7789 initialization sequence from Python to C
- Implement `flush_cb` with SPI transfer
- Test: solid color fill on real hardware
- Test: LVGL `lv_label` "Hello World" rendering

### Phase 2: Input Driver
- Implement `indev_buttons.c` with sysfs GPIO reading
- Configure LVGL keypad input device
- Set up `lv_group` for focus navigation
- Test: navigate between widgets with D-pad
- Test: select/back with A/B buttons

### Phase 3: Screen — Status
- Implement `scr_status.c` with date/time/IP labels
- Add `lv_timer` for 1-second refresh
- Port `get_ipv4()` to C (socket ioctl)
- Style with dark theme (black bg, white/cyan text)

### Phase 4: Screen — Eyes Animation
- Port `EyesState` from Python to C (`eyes_anim.c`)
- Implement `lv_canvas`-based rendering for ellipses
- Port gaze easing, auto-gaze random targeting
- Port blink animation with `lv_anim`
- Target 30+ FPS on hardware

### Phase 5: IPC Server
- Implement Unix Domain Socket server (`ipc_server.c`)
- Implement JSON-line command parser (`cmd_parser.c`)
- Handle screen switching commands
- Handle text/image data transfer
- Forward X/Y button events to Python

### Phase 6: Python Server V2
- Create `gui_client.py` IPC client module
- Refactor `http_api_server.py` to remove all display code
- Route all display API calls through `GUIClient`
- Verify all existing HTTP API endpoints work unchanged
- Test with existing `client/` code

### Phase 7: Remaining Screens
- Implement `scr_emoji.c` (load PNG from SD card)
- Implement `scr_text.c` (dynamic label)
- Implement `scr_image.c` (load binary from /tmp)
- Implement `scr_menu.c` (list-based navigation)
- Add screen transition animations

### Phase 8: Theme & Polish
- Create `lf_theme.c` custom dark theme
- Fine-tune font sizes for 240x240 readability
- Add boot splash screen
- Add error/disconnected state indicators
- Performance profiling and optimization

### Phase 9: Framebuffer Migration (Optional)
- Build custom kernel with fbtft support
- Create device tree overlay for ST7789 framebuffer
- Switch `disp_driver.c` to mmap framebuffer mode
- Benchmark framebuffer vs direct SPI performance
- Update init scripts

### Phase 10: Integration Testing & Deployment
- End-to-end testing of all API endpoints
- Button navigation testing across all screens
- Stress testing (rapid screen switches, large images)
- Memory leak testing (valgrind on host, long-run on target)
- Update `sync.sh` with V2 file mappings
- Final deployment and boot verification

---

## 14. Risk Analysis & Mitigations

### 14.1 Technical Risks

| Risk | Impact | Probability | Mitigation |
|------|--------|-------------|------------|
| Kernel FB driver unavailable for RV1106 | Can't use `/dev/fb0` | Medium | Direct SPI mode as primary approach; FB is optional optimization |
| LVGL binary too large for flash | Can't deploy | Low | Strip symbols, disable unused features, store on SD card |
| SPI contention between LVGL and spidev | Display corruption | High (if both run) | V2 design ensures only LVGL binary touches SPI; Python never accesses display |
| IPC latency affects API response time | Slow API | Low | UDS latency is ~50μs; negligible vs HTTP overhead |
| LVGL memory usage exceeds RAM budget | OOM kills | Medium | Tune `LV_MEM_SIZE`, use partial rendering (20-line buffer), profile early |
| Eyes animation quality regression | Visual downgrade | Medium | Port exact algorithm to C; canvas gives same pixel-level control |
| Cross-compiler ABI mismatch | Binary won't run | Low | Use exact SDK toolchain; test on hardware in Phase 1 |

### 14.2 Compatibility Matrix

| V1 Feature | V2 Status | Notes |
|------------|-----------|-------|
| GET /api/status | Compatible | Routed via IPC |
| GET /api/mode/status | Compatible | IPC screen switch |
| GET /api/mode/eyes | Compatible | IPC screen switch |
| GET /api/emoji/{name} | Compatible | IPC + PNG from SD |
| POST /api/text | Compatible | IPC text command |
| POST /api/image | Compatible | File + IPC notify |
| POST /api/gif/frames | Compatible | Sequential IPC frames |
| GET /api/capture | Compatible | Unchanged (camera) |
| Audio endpoints | Compatible | Unchanged (UART) |
| Button A toggle | Changed | Now LV_KEY_ENTER (select) |
| D-pad gaze control | Changed | D-pad is LVGL navigation; gaze via IPC zone |
| Face tracking zones | Compatible | Python reads file, sends via IPC |

---

## Appendix A: Quick Reference — File Change Summary

| File | Action | Description |
|------|--------|-------------|
| `lvgl_gui/` (entire tree) | **NEW** | LVGL C application |
| `board/sdcard/gui_client.py` | **NEW** | Python IPC client |
| `board/sdcard/http_api_server.py` | **REWRITE** | Remove display code, add IPC |
| `board/root/main.py` | **MODIFY** | Wait for GUI, then start Python |
| `board/init.d/S99python` | **MODIFY** | Launch luckfox_gui first |
| `board/executables/luckfox_gui` | **NEW** | Compiled ARM binary |
| `board/sdcard/emojis/` | **NEW** | PNG emoji assets |
| `board/sdcard/display_eyes.py` | **DEPRECATED** | Replaced by scr_eyes.c |
| `sync.sh` | **MODIFY** | Add V2 file mappings |

## Appendix B: Memory Budget (RV1106, 256MB DDR3)

| Component | Estimated Usage |
|-----------|----------------|
| Linux kernel + rootfs | ~30MB |
| LVGL binary (code + stack) | ~1MB |
| LVGL heap (`LV_MEM_SIZE`) | 48KB |
| LVGL draw buffer (20 lines) | 9.6KB (240*20*2) |
| LVGL widget tree (all screens) | ~20KB |
| Python interpreter | ~8MB |
| HTTP server + audio | ~5MB |
| **Total estimated** | **~45MB** |
| **Available** | **~211MB free** |

## Appendix C: Performance Targets

| Metric | V1 (Python) | V2 (LVGL) Target |
|--------|-------------|-------------------|
| Eyes animation FPS | ~25 | 30+ |
| Status screen refresh | 1 Hz | 1 Hz (timer) |
| Screen switch latency | ~200ms | <50ms |
| API response time | ~50ms | ~60ms (+ IPC) |
| Boot to display | ~5s | ~3s |
| Binary size | N/A (Python) | <500KB |
| RAM usage (display) | ~15MB (Python) | ~2MB (C) |
