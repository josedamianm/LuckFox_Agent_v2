# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

LuckFox Agent V2 is a dual-process embedded GUI system for the **LuckFox Pico Max** (Rockchip RV1106, ARM Cortex-A7) with a 240×240 SPI display (Waveshare Pico LCD 1.3", ST7789). It consists of:

- **C binary** (`luckfox_gui`): Owns the Linux framebuffer (`/dev/fb0`) and drives LVGL v9 GUI
- **Python HTTP API server**: Bridges external HTTP requests to the C binary via Unix Domain Socket IPC
- **ESP32-C3 Arduino sketch**: Audio I2S receiver communicating over UART2

## Build System

The C binary uses CMake with a cross-compilation toolchain for ARM Cortex-A7.

### Cross-compile (inside Docker `luckfox-crossdev:1.0`)

```bash
# First-time LVGL submodule setup
git submodule add https://github.com/lvgl/lvgl.git lvgl_gui/lib/lvgl
cd lvgl_gui/lib/lvgl && git checkout release/v9.2 && cd ../../..

# Build
mkdir -p lvgl_gui/build && cd lvgl_gui/build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Output: lvgl_gui/build/luckfox_gui
```

The toolchain (`toolchain-rv1106.cmake`) looks for the ARM compiler at `TOOLCHAIN_PATH`, `LUCKFOX_SDK`, or `/toolchain` (Docker default). Target flags: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2`.

### Deployment

```bash
./sync.sh push    # rsync files to board at 192.168.1.60
./sync.sh pull    # pull files from board
./sync.sh diff    # show differences
./sync.sh status  # show sync status
```

Board file locations:
- Binary: `/usr/local/bin/luckfox_gui`
- Python scripts: `/mnt/sdcard/`
- Assets: `/mnt/sdcard/emoji/` and `/mnt/sdcard/images/`

## Architecture

### Dual-Process Communication

```
HTTP clients (port 8080)
        ↓
http_api_server_v2.py  →  gui_client.py
                               ↓
                   /tmp/luckfox_gui.sock  (JSON, newline-delimited)
                               ↓
                        luckfox_gui (C binary)
                               ↓
                    LVGL → /dev/fb0 → ST7789 display
```

### IPC Protocol

Newline-delimited JSON over Unix domain socket `/tmp/luckfox_gui.sock`:
- Commands (Python → C): `{"cmd": "screen", "name": "status"}\n`
- Events (C → Python): `{"event": "button", "name": "A", "state": "pressed"}\n`

### C Binary Structure (`lvgl_gui/src/`)

| Module | Role |
|--------|------|
| `main.c` | LVGL init, event loop (5ms tick), signal handling |
| `hal/disp_driver.c` | Framebuffer flush to 240×240 RGB565 display |
| `hal/indev_buttons.c` | 9 GPIO buttons → LVGL input events |
| `ipc/ipc_server.c` | Unix socket server, up to 4 clients, non-blocking |
| `ipc/cmd_parser.c` | JSON command → screen handler dispatch |
| `screens/scr_manager.c` | Screen switching with 200ms fade animation |
| `screens/scr_eyes.c` | Animated eyes with 9-zone gaze control |
| `screens/scr_emoji.c` | PNG display from `/mnt/sdcard/emoji/` |
| `anim/eyes_anim.c` | Eye gaze/blink animations |

### Python Components (`board/sdcard/`)

- **`http_api_server_v2.py`**: HTTP API on port 8080. Key endpoints: `GET /api/mode/{status,eyes}`, `POST /api/text`, `POST /api/image`, `POST /api/gif/frames`, `POST /api/audio/play`
- **`gui_client.py`**: Thread-safe IPC client with auto-reconnect (0.5s backoff)
- **`audio_sender.py`**: UART2 protocol to ESP32-C3 — binary framing with SYNC `0xAA55`, packet type, length, payload, XOR checksum @ 921600 baud

### LVGL Configuration (`lvgl_gui/lv_conf.h`)

- Color depth: 16-bit RGB565
- Memory buffer: 48KB
- DPI: 200, display size: 240×240
- Enabled fonts: Montserrat 12, 16, 24, 32

## Key Reference Docs

- `ARCHITECTURE_V2_LVGL.md` — Full system design, IPC protocol spec, screen designs, boot sequence
- `DEPLOY_GUIDE.md` — Docker cross-compile setup, board deployment, troubleshooting
- `lvgl_gui/README.md` — LVGL submodule setup and build steps

## Screens

Six screen types managed by `scr_manager`: `STATUS`, `EYES`, `EMOJI`, `TEXT`, `IMAGE`, `MENU`

## Board Hardware

- **Display**: SPI0 (`/dev/spidev0.0`), enabled via `enable_spi0_spidev.dtbo`
- **Buttons**: 9 GPIO buttons (A, B, X, Y, UP, DOWN, LEFT, RIGHT, CTRL)
- **Audio**: UART2 (`/dev/ttyS2`) → ESP32-C3 → I2S DAC
- **Camera**: Captured via `board/executables/get_frame.c`
- **Remote access**: frpc tunnel (`board/init.d/S98frpc`)
