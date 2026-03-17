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
| `screens/scr_chat.c` | AI Chat screen — 4 animated states (IDLE/LISTENING/THINKING/SPEAKING) |
| `anim/eyes_anim.c` | Eye gaze/blink animations |

### Python Components (`board/sdcard/`)

- **`http_api_server_v2.py`**: HTTP API on port 8080. Key endpoints: `GET /api/mode/{status,eyes,chat}`, `POST /api/text`, `POST /api/image`, `POST /api/gif/frames`, `POST /api/audio/play`, `POST /api/chat/state`
- **`gui_client.py`**: Thread-safe IPC client with auto-reconnect (0.5s backoff)
- **`audio_sender.py`**: UART2 protocol to ESP32-C3 — binary framing with SYNC `0xAA55`, packet type, length, payload, XOR checksum @ 921600 baud

### LVGL Configuration (`lvgl_gui/lv_conf.h`)

- Color depth: 16-bit RGB565
- Memory buffer: 48KB
- DPI: 200, display size: 240×240
- Enabled fonts: Montserrat 12, 16, 24, 32, 48

## Key Reference Docs

- `ARCHITECTURE_V2_LVGL.md` — Full system design, IPC protocol spec, screen designs, boot sequence
- `DEPLOY_GUIDE.md` — Docker cross-compile setup, board deployment, troubleshooting
- `lvgl_gui/README.md` — LVGL submodule setup and build steps

## Screens

Seven screen types managed by `scr_manager`: `STATUS`, `EYES`, `EMOJI`, `TEXT`, `IMAGE`, `MENU`, `CHAT`

### AI Chat Screen (`SCR_CHAT`)

Four states driven by IPC commands or HTTP API:

| State | Value | Visual | Color |
|-------|-------|--------|-------|
| `CHAT_IDLE` | 0 | Large mic symbol + "HOLD BUTTON" | Gray `#444` |
| `CHAT_LISTENING` | 1 | 7 animated waveform bars (ping-pong) | Green `#00FF80` |
| `CHAT_THINKING` | 2 | Spinning partial arc (300° fill) | Orange `#FF8800` |
| `CHAT_SPEAKING` | 3 | Scrolling response text + dot indicators | Cyan `#00CCFF` |

IPC commands:
- `{"cmd": "screen", "name": "chat"}` — navigate to chat screen
- `{"cmd": "chat_state", "state": 0}` — set state (0–3)
- `{"cmd": "chat_text", "text": "..."}` — set speaking text

HTTP endpoints:
- `GET /api/mode/chat` — switch to chat screen
- `POST /api/chat/state` — body: `{"state": 1, "text": "optional response text"}`

### Menu Screen (`SCR_MENU`)

6-card horizontal tileview (swipe or LEFT/RIGHT buttons, ENTER to enter):
`Eyes → Status → Emoji → Text → Image → AI Chat`

Dot indicator bar: 6 pills at bottom (active = 14px wide accent-colored, inactive = 7px gray).

### Design System

Smartwatch-style, all screens use:
- Pure black `#000000` background
- Font scale: 48px primary value, 32px secondary, 24px labels, 16px body, 12px meta
- One dominant element per screen, no persistent navigation chrome
- `lv_font_montserrat_48` must be enabled in `lv_conf.h` (`LV_FONT_MONTSERRAT_48 1`)

## Board Hardware

- **Display**: SPI0 (`/dev/spidev0.0`), enabled via `enable_spi0_spidev.dtbo`
  - ST7789 240×240, MADCTL `0x60` (90° landscape), XOFF=80, YOFF=0
  - SPI uses `write()` syscall (not `ioctl SPI_IOC_MESSAGE`) — required on RK1106
  - RGB565 big-endian: manual byte swap done in `flush_cb`
  - LVGL render mode: `LV_DISPLAY_RENDER_MODE_FULL` with single 240×240 buffer
  - Refresh: call `lv_refr_now(NULL)` to force immediate flush after state changes
- **Buttons**: 9 GPIO buttons, active-low (idle=1, pressed=0), sysfs polling
  - A=GPIO57, B=GPIO69, X=GPIO65, Y=GPIO67
  - UP=GPIO55, DOWN=GPIO64, LEFT=GPIO68, RIGHT=GPIO66, CTRL=GPIO54
  - GPIOs must be exported and set to `in` direction by the binary on startup
  - LVGL keypad indev `read_cb` not called in v9 — use direct sysfs polling in main loop
- **Audio**: UART2 (`/dev/ttyS2`) → ESP32-C3 → I2S DAC
  - TX=GPIO42 (Pin1) → ESP32-C3 GPIO4, RX=GPIO43 (Pin2) ← ESP32-C3 GPIO7, GND↔GND
  - 921600 baud, binary framing: SYNC `0xAA55`, packet type, length, payload, XOR checksum
- **Camera**: Captured via `board/executables/get_frame.c`
- **Remote access**: frpc tunnel (`board/init.d/S98frpc`)

## Current State (as of 2026-03-17)

The `main.c` is currently a **color-test binary** (not the full app) that validates display + all 9 buttons:
- Boot: blue diagnostic flash → navy screen with button instructions label
- Press A=Red, B=Blue, X=Yellow, Y=Green, UP=Orange, DOWN=Purple, LEFT=Cyan, RIGHT=White, CTRL=Magenta
- Direct GPIO sysfs polling in main loop + `lv_refr_now(NULL)` on each button press

Next step: restore full app (IPC server, screen manager, HTTP agent integration).

## Known Issues / Lessons Learned

- `lv_timer_handler()` blocks in LVGL v9 partial render mode → use `LV_DISPLAY_RENDER_MODE_FULL`
- `LV_COLOR_16_SWAP` is a v8 macro ignored by LVGL v9 → do manual byte swap in `flush_cb`
- `LV_COLOR_FORMAT_RGB565_SWAP` enum does not exist in this build → use `LV_COLOR_FORMAT_RGB565`
- LVGL v9 keypad indev `read_cb` not polled unless a focused group/object exists → bypass with direct GPIO polling
- ST7789 on RK1106 spidev: `ioctl(SPI_IOC_MESSAGE)` fails silently → use `write()` syscall
- GPIO buttons: export + set direction `in` on every startup (not persistent across reboots)
- Initialize button `prev[]` state from actual GPIO reads to avoid false triggers on startup
