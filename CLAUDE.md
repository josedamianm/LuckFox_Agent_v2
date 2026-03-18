# CLAUDE.md — LuckFox Agent V2

Single source of truth for AI agents working on this project. Read this file completely before making any changes.

---

## 1. Project Overview

LuckFox Agent V2 is a voice-activated AI agent running on the **LuckFox Pico Max** (Rockchip RV1106 / ARM Cortex-A7, 256MB DDR3). It uses a dual-process architecture:

- **`luckfox_gui`** (C binary): drives LVGL v9 GUI on a 240×240 ST7789 SPI display, owns GPIO buttons, emits button events over IPC
- **`http_api_server_v2.py`** (Python): HTTP API on port 8080, AI pipeline coordination (STT → LLM → TTS via MacBook), drives C binary state via IPC

```
User presses CTRL button
        ↓
luckfox_gui (C) → IPC event → Python HTTP server
                                    ↓
                          HTTP → MacBook pipeline
                          (audio → STT → LLM → TTS)
                                    ↓
                    IPC command → luckfox_gui (C)
                                    ↓
                        LVGL → SPI → ST7789 240×240
```

### Why Dual-Process?

1. **Performance**: C + LVGL renders at native speed vs Python's ~40ms/frame
2. **LVGL threading**: LVGL is not thread-safe; isolated process avoids sync issues
3. **Stability**: Python crash doesn't kill GUI; display keeps showing last screen
4. **Separation**: GUI rendering vs API/network/audio are fundamentally different workloads

---

## 2. Current Status (2026-03-18)

**Display + all 9 buttons confirmed working on hardware.**

**IPC/HTTP integration complete.** All three Python files speak the V2 protocol:
- `gui_client.py`: `set_state()` sends `{"cmd": "set_state", "state": "...", "text": "..."}` matching `cmd_parser.c`
- `http_api_server_v2.py`: V2 agent state endpoints (`GET/POST /api/agent/state`), CTRL button events drive state machine (pressed→LISTENING, released→THINKING)
- `main.py`: launches `http_api_server_v2.py`

**IDLE screen confirmed working on hardware:**
- Page 0 (Status): time updates every second, date and IP display correctly
- Page 1 (Kawaii face): smooth animation, blink/bounce/emotion state machine working
- LEFT/RIGHT buttons navigate between pages
- Clock uses `time()`/`localtime()`/`strftime()` — pure C stdlib, no shell required

**Camera confirmed working (2026-03-18):**
- rkipc cycle snapshots writing to `/mnt/sdcard` every 200ms (~5 FPS)
- `/api/capture` returns complete JPEG frames (<100ms response)
- `/api/stream` MJPEG stream working at ~5 FPS in browser/VLC
- Cleanup daemon keeps only 5 snapshots on SD card at all times
- All client commands verified: status, state, set, capture, camera-status, stream, tone, audio, audio-stop

**Next step**: Connect the MacBook AI pipeline — when THINKING state is entered (CTRL released), send recorded audio to MacBook for STT → LLM → TTS, then set SPEAKING state with response text, then IDLE when playback completes.

---

## 3. Agent State Machine

Five states. The C binary renders the correct screen for each state. Python drives state transitions via IPC.

| State | Trigger | Visual |
|-------|---------|--------|
| `IDLE` | Boot / pipeline complete | Logo + "Press CTRL to start" |
| `LISTENING` | CTRL pressed | Mic/waveform animation, green |
| `THINKING` | CTRL released | Spinner/dots, orange |
| `SPEAKING` | LLM response ready | Response text + waveform, cyan |
| `ERROR` | Any failure | Error message, red |

```
IDLE ──[CTRL press]──► LISTENING ──[CTRL release]──► THINKING
                                                          │
                                              [response ready]
                                                          ▼
IDLE ◄──[playback done]── SPEAKING ◄──[LLM done]── THINKING
                                                          │
IDLE ◄──────────────── ERROR ◄────────[any failure]──────┘
```

---

## 4. Screen Layouts (240×240, dark background)

### IDLE
- Centered brand text: `"LUCKFOX AGENT"` — Montserrat 32px, white
- Sub-text: `"Press CTRL to start"` — 16px, gray `#888888`
- Background: pure black `#000000`

### LISTENING
- Top label: `"Listening..."` — 24px, green `#00FF80`
- Center: 7 animated vertical waveform bars (ping-pong height animation), green `#00FF80`
- Bottom label: `"Release CTRL to process"` — 14px, gray `#888888`

### THINKING
- Center: spinning arc animation (300° arc rotates), orange `#FF8800`
- Label below spinner: `"Thinking..."` — 24px, orange `#FF8800`

### SPEAKING
- Top label: `"Speaking..."` — 24px, cyan `#00CCFF`
- Center text area: response text (last ~120 chars that fit), 16px, white, word-wrap
- Bottom: 3 animated pulsing dots, cyan `#00CCFF`

### ERROR
- Center: `"!"` symbol — 48px, red `#FF3333`
- Below: error message text — 16px, red `#FF3333`, truncated to fit

---

## 5. IPC Protocol

Newline-delimited JSON over Unix domain socket `/tmp/luckfox_gui.sock`. Transport: `SOCK_STREAM`, bidirectional. Python client auto-reconnects with 500ms backoff. Up to 4 simultaneous clients.

### Python → C (commands)

```json
{"cmd": "set_state", "state": "idle"}
{"cmd": "set_state", "state": "listening"}
{"cmd": "set_state", "state": "thinking"}
{"cmd": "set_state", "state": "speaking", "text": "response text here"}
{"cmd": "set_state", "state": "error", "text": "error message"}
```

### C → Python (events)

```json
{"event": "button", "name": "CTRL", "state": "pressed"}
{"event": "button", "name": "CTRL", "state": "released"}
```

---

## 6. C Binary Structure (`lvgl_gui/src/`)

| Module | Role |
|--------|------|
| `main.c` | LVGL init, main loop, GPIO button polling, signal handling |
| `hal/disp_driver.c` | ST7789 SPI driver — init, flush_cb (byte swap via `write()` syscall) |
| `hal/disp_driver.h` | Public: `disp_driver_init()`, `disp_driver_deinit()`, `disp_fill_color()` |
| `ipc/ipc_server.c` | Unix socket server, non-blocking, up to 4 clients, `ipc_server_broadcast()` |
| `ipc/ipc_server.h` | Public: `ipc_server_init()`, `ipc_server_poll()`, `ipc_broadcast()` |
| `ipc/cmd_parser.c` | Parse `set_state` JSON → call `agent_set_state()` |
| `ipc/cmd_parser.h` | Public: `cmd_parse()` |
| `screens/scr_agent.c` | All 5 agent states in one file — tick-driven animations |
| `screens/scr_agent.h` | Public: `agent_screen_init()`, `agent_set_state()`, `agent_tick()` |

### `main.c` Main Loop

```c
lv_init();
disp_driver_init();
agent_screen_init();       // creates LVGL screen, sets IDLE state
ipc_server_init();         // binds /tmp/luckfox_gui.sock
gpio_buttons_init();       // export + set direction, init prev[] from reads

while (running) {
    ipc_server_poll();     // accept new clients, read commands, dispatch
    gpio_poll_buttons();   // detect CTRL press/release → ipc_broadcast() event
    agent_tick();          // drive animations (waveform bars, spinner, dots)
    lv_timer_handler();    // LVGL render cycle
    usleep(10000);         // 10ms = 100Hz loop
}
```

### `scr_agent.c` Structure

Single LVGL screen with overlapping containers, one shown per state:

```c
typedef enum { STATE_IDLE, STATE_LISTENING, STATE_THINKING, STATE_SPEAKING, STATE_ERROR } agent_state_t;

void agent_screen_init(void);           // create all 5 state containers on one lv_screen
void agent_set_state(agent_state_t s, const char *text);  // hide all, show target, start anim
void agent_tick(void);                  // called every loop iteration — advance animations
```

Animation approach (no LVGL anim timers, driven by `agent_tick()`):
- **LISTENING bars**: 7 `lv_obj` rectangles, heights oscillate with per-bar phase offset
- **THINKING spinner**: single `lv_arc`, start angle incremented by 4° per tick
- **SPEAKING dots**: 3 circles, opacity pulses with phase offset per dot

---

## 7. Python-Side Files

| File | Role |
|------|------|
| `board/sdcard/gui_client.py` | IPC client class — connects to `/tmp/luckfox_gui.sock`, auto-reconnects, sends JSON, reads button events in background thread |
| `board/sdcard/http_api_server_v2.py` | HTTP API on port 8080 — agent state, camera capture/stream, audio playback via ESP32-C3 |
| `board/root/main.py` | Boot launcher — starts `luckfox_gui` binary, waits 1.5s, then starts Python server |
| `board/sdcard/audio_sender.py` | UART packet protocol to ESP32-C3 |
| `client/luckfox_remote.py` | MacBook CLI client for testing all API endpoints |

---

## 8. Hardware Configuration (Confirmed Working)

### Display — ST7789 240×240

| Item | Value |
|------|-------|
| SPI device | `/dev/spidev0.0` @ 32MHz, `SPI_MODE_0` |
| SPI transfer | **`write()` syscall** — `ioctl(SPI_IOC_MESSAGE)` silently fails on RV1106 |
| DC GPIO | 73 |
| RST GPIO | 51 |
| BL GPIO | 72 |
| MADCTL | `0x60` (90° landscape) |
| Window offset | XOFF=80, YOFF=0 |
| Color format | RGB565 big-endian — manual byte swap in `flush_cb` |
| LVGL render mode | `LV_DISPLAY_RENDER_MODE_FULL`, single 240×240 buffer, `NULL` second buffer |
| LVGL refresh | `lv_refr_now(NULL)` for immediate flush; `lv_timer_handler()` in main loop |

### Buttons — 9 GPIO, active-low (idle=1, pressed=0)

| Button | GPIO |
|--------|------|
| A | 57 |
| B | 69 |
| X | 65 |
| Y | 67 |
| UP | 55 |
| DOWN | 64 |
| LEFT | 68 |
| RIGHT | 66 |
| CTRL | 54 |

- GPIOs must be **exported** and set to `in` direction by the binary on every startup (not persistent across reboots)
- Use **direct sysfs polling** in main loop (LVGL v9 keypad indev `read_cb` not polled without focused group)
- Initialize `prev[]` state from actual GPIO reads to prevent false triggers on startup
- Only CTRL button events are emitted over IPC (others reserved for future use)
- Hardware pull-ups configured via IOC registers (`board/init.d/S99button_pullups`)

### Audio — ESP32-C3 via UART2

| Item | Value |
|------|-------|
| Device | `/dev/ttyS2` |
| Baud | 921600 |
| Protocol | Binary: SYNC `0xAA55`, packet type, length, payload, XOR checksum |
| LuckFox TX | GPIO42 (Pin1) → ESP32-C3 GPIO4 |
| LuckFox RX | GPIO43 (Pin2) ← ESP32-C3 GPIO7 |

---

## 9. LVGL v9 Critical Notes

These are hard-won lessons. Do NOT change these without testing on hardware:

- `lv_timer_handler()` **blocks** in partial render mode → must use `LV_DISPLAY_RENDER_MODE_FULL`
- `LV_COLOR_16_SWAP 1` is a v8 macro, **silently ignored** in v9 → manual byte swap in `flush_cb`
- `LV_COLOR_FORMAT_RGB565_SWAP` enum **does not exist** in this build → use `LV_COLOR_FORMAT_RGB565`
- LVGL v9 keypad indev requires a focused group for `read_cb` to be polled → bypass with direct GPIO loop
- Call `lv_refr_now(NULL)` after style/content changes to force immediate screen refresh
- Prefer tick-driven animations in `agent_tick()` over LVGL anim timers for predictability
- **`popen("date ...")`  silently fails on RV1106** (no shell at that path) → always use `time()`/`localtime()`/`strftime()` for system time
- **`lv_timer_ready()`** may not exist in all LVGL v9 builds → call the callback directly instead
- **Clock update pattern (confirmed working)**: call `update_clock()` inside `agent_tick()` every tick; use a static `time_t g_last_second` to detect second changes; call `lv_refr_now(NULL)` every tick (same as face animation page)

### LVGL Configuration (`lvgl_gui/lv_conf.h`)

- `LV_COLOR_DEPTH 16` (RGB565)
- `LV_COLOR_16_SWAP 0` (ignored in v9; byte swap done manually)
- Buffer: 48KB, DPI: 200, display: 240×240
- Fonts enabled: Montserrat 12, 16, 24, 32, 48
- Tick: custom via `gettimeofday()`
- File system: POSIX, letter `S`, path `/mnt/sdcard`

---

## 10. Build System

### Cross-Compilation

Uses Docker container `luckfox-crossdev:1.0` with Rockchip toolchain at `/toolchain`.

**Toolchain**: `arm-rockchip830-linux-uclibcgnueabihf-gcc`
**Flags**: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2`
**Auto-detection order**: `TOOLCHAIN_PATH` env → `LUCKFOX_SDK` env → `/toolchain`

#### Start Docker Container

```bash
docker run -it --name luckfox-crossdev \
  -v ~/luckfox-crossdev/projects:/workspace \
  luckfox-crossdev:1.0 /bin/bash

# If already exists:
docker start -i luckfox-crossdev
```

#### LVGL Submodule Setup (first time only)

```bash
git submodule add https://github.com/lvgl/lvgl.git lvgl_gui/lib/lvgl
cd lvgl_gui/lib/lvgl && git checkout release/v9.2 && cd ../../..

# Or if .gitmodules already exists:
git submodule update --init --recursive
cd lvgl_gui/lib/lvgl && git checkout release/v9.2 && cd ../../..
```

#### Build

```bash
cd lvgl_gui
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Output: lvgl_gui/build/luckfox_gui (ELF 32-bit ARM)
```

#### Verify Binary

```bash
file lvgl_gui/build/luckfox_gui
# Expected: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV)
```

#### Rebuild After Changes

The Docker container runs on a remote Ubuntu server (`nlighten_gpu_server`). The workspace is mounted at `/home/josemanco/luckfox-crossdev/projects/` on the server and at `/workspace/` inside the container. Follow these steps end-to-end whenever the C binary (`luckfox_gui`) needs to be recompiled.

##### Step 1 — Push changes from MacBook to GitHub

```bash
git add -A && git commit -m "your message"
git push
```

##### Step 2 — Start container on remote server and open a shell

```bash
ssh nlighten_gpu_server
docker start luckfox-crossdev && docker exec -it luckfox-crossdev /bin/bash
```

##### Step 3 — Pull latest code inside container

```bash
cd /workspace/LuckFox_Agent_v2
git pull
```

##### Step 4 — Compile (clean build)

```bash
cd lvgl_gui
rm -rf build && mkdir build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake .. && make -j$(nproc)
ls -ltrh   # verify luckfox_gui binary was produced
```

##### Step 5 — Copy binary back to MacBook

```bash
# Run on MacBook (not inside container/server)
cd /path/to/LuckFox_Agent_v2
scp nlighten_gpu_server:/home/josemanco/luckfox-crossdev/projects/LuckFox_Agent_v2/lvgl_gui/build/luckfox_gui board/executables/luckfox_gui
ls -ltrh board/executables/luckfox_gui   # verify
```

##### Step 6 — Push binary to board via sync.sh

```bash
./sync.sh push
```

##### Step 7 — Reboot board to apply

```bash
ssh root@192.168.1.60 "reboot"
# or via tunnel:
# ssh root@luckfoxpico1.aiserver.onmobilespace.com "reboot"
```

---

## 11. Deployment

### Sync Script (preferred)

```bash
./sync.sh              # rsync push to board (remote via SSH tunnel)
./sync.sh --private    # push via local network (192.168.1.60)
./sync.sh pull         # pull from board
./sync.sh diff         # show differences
./sync.sh status       # show sync status
```

Remote: `root@luckfoxpico1.aiserver.onmobilespace.com:8022`
Local: `root@192.168.1.60`

### Manual Deploy

```bash
scp lvgl_gui/build/luckfox_gui root@192.168.1.60:/root/Executables/luckfox_gui
ssh root@192.168.1.60 "killall luckfox_gui; /root/Executables/luckfox_gui"
```

### Board File Locations

| Item | Path |
|------|------|
| C binary | `/root/Executables/luckfox_gui` |
| Python scripts | `/mnt/sdcard/` |
| IPC socket | `/tmp/luckfox_gui.sock` |
| Autostart | `/etc/init.d/S99luckfox_agent` |
| Emoji PNGs | `/mnt/sdcard/emoji/<name>.png` |
| HTTP API port | `8080` |

### Boot Sequence

```
S20spi0overlay          ← Enable /dev/spidev0.0
S21uart2overlay         ← Enable UART2 for audio
S50rtcinit              ← RTC initialization
S60ntpd                 ← NTP time sync
S98frpc                 ← FRP tunnel
S99button_pullups       ← GPIO pull-up registers
S99luckfox_agent        ← Launch luckfox_gui, then Python server
```

### Autostart Script

```sh
#!/bin/sh
start() {
    /root/Executables/luckfox_gui &
    sleep 2
    python3 /mnt/sdcard/http_api_server_v2.py &
}
stop() { pkill luckfox_gui; pkill -f http_api_server_v2.py; }
case "$1" in
    start) start ;; stop) stop ;; restart) stop; sleep 1; start ;;
esac
```

---

## 12. Camera — rkipc Integration

The camera is managed entirely by the **rkipc** service (Rockchip IPC daemon). Do NOT run a custom camera_daemon — it would conflict with rkipc and lose ISP/3A processing, producing dark/greenish images.

### How rkipc works

- Started automatically at boot via `RkLunch.sh` → `post_chk()` → `rkipc -a /oem/usr/share/iqfiles`
- Reads config from `/userdata/rkipc.ini` (copied from `/oem/usr/share/rkipc-300w.ini` on every boot)
- Provides RTSP stream at `rtsp://<device-ip>/live/0` and `rtsp://<device-ip>/live/1`
- With `enable_cycle_snapshot = 1`: saves JPEG files to `/mnt/sdcard/` every `snapshot_interval_ms` ms
- JPEG filename format: `YYYYMMDDHHMMSS.jpeg` in subdirectories under `mount_path`
- IPC socket: `/var/tmp/rkipc` (Unix domain socket, `srw-rw-rw-`)

### Key config (persisted in `/oem/usr/share/rkipc-300w.ini`)

**IMPORTANT**: Always edit `/oem/usr/share/rkipc-300w.ini` for persistent changes. `/userdata/rkipc.ini` is overwritten on every boot by `RkLunch.sh`.

```ini
[video.source]
enable_jpeg = 1
enable_rtsp = 1

[video.jpeg]
width = 1920
height = 1080
enable_cycle_snapshot = 1
snapshot_interval_ms = 200      ; ~5 FPS — tune for speed vs CPU load

[storage]
mount_path = /mnt/sdcard        ; NOT /userdata (only 2.2MB, always full)
free_size_del_min = 50          ; MB — start deleting old files below this
free_size_del_max = 200         ; MB — stop deleting above this
```

### Python API (`http_api_server_v2.py`)

| Constant / Function | Behaviour |
|---|---|
| `RKIPC_SNAPSHOT_DIR = "/mnt/sdcard"` | Root dir walked for snapshot files |
| `SNAPSHOT_KEEP = 5` | Max snapshots kept by cleanup daemon |
| `rkipc_running()` | Returns `True` if `/var/tmp/rkipc` exists as a socket |
| `latest_snapshot()` | Walks `RKIPC_SNAPSHOT_DIR` tree, returns path + mtime of newest `.jpeg` |
| `jpeg_complete(data)` | Checks `FFD8` header and `FFD9` in last 64 bytes (avoids partial reads) |
| `capture_frame()` | Retries up to 2s for a complete JPEG; follows newer files if they appear |
| `cleanup_snapshots()` | Background thread (every 5s): keeps newest 5 snapshots, deletes the rest |
| `GET /api/capture` | Returns latest complete JPEG as `image/jpeg` |
| `GET /api/camera/status` | Returns `rkipc_running`, `rtsp_url`, `latest_snapshot`, `snapshot_age_sec` |
| `GET /api/stream` | MJPEG multipart stream — polls snapshot dir, serves each new file as a frame |

### Disk management

At 200ms interval, rkipc writes ~5 JPEGs/sec (~130KB each). The `cleanup_snapshots()` daemon runs every 5s and keeps only the 5 newest files (~650KB total on SD card). The SD card (`/mnt/sdcard`, ~60GB) has plenty of headroom.

### RTSP stream (VLC / external)

```
rtsp://<device-ip>/live/0   ← main stream (1920×1080)
rtsp://<device-ip>/live/1   ← sub stream
```

---

## 14. V1 Reference (Legacy)

Key V1 files still in the repo (deprecated):

| File | Description |
|------|-------------|
| `board/sdcard/http_api_server.py` | V1 monolith (replaced by `http_api_server_v2.py`) |
| `board/init.d/S99python` | V1 boot script (starts `main.py` which launches the old server) |

---

## 14. Project Directory Structure

```
LuckFox_Agent_v2/
├── CLAUDE.md                        ← This file (single source of truth)
├── sync.sh                          ← rsync deploy script
│
├── board/                           ← Files deployed to the board
│   ├── root/
│   │   ├── main.py                  ← Boot launcher (starts luckfox_gui + Python server)
│   │   ├── enable_spi0_spidev.dtbo
│   │   ├── enable_spi0_spidev.dts
│   │   └── enable_uart2.dts
│   ├── sdcard/
│   │   ├── http_api_server_v2.py    ← V2 HTTP API server (IPC-based)
│   │   ├── gui_client.py            ← Python IPC client class
│   │   ├── audio_sender.py          ← UART audio to ESP32-C3
│   │   └── http_api_server.py       ← V1 legacy (deprecated)
│   ├── executables/
│   │   └── luckfox_gui              ← Compiled ARM binary
│   └── init.d/
│       ├── S20spi0overlay
│       ├── S21uart2overlay
│       ├── S50rtcinit
│       ├── S60ntpd
│       ├── S98frpc
│       ├── S99button_pullups
│       └── S99python
│
├── lvgl_gui/                        ← LVGL C application
│   ├── CMakeLists.txt
│   ├── toolchain-rv1106.cmake
│   ├── lv_conf.h
│   ├── src/
│   │   ├── main.c
│   │   ├── hal/
│   │   │   ├── disp_driver.c       ← ST7789 SPI driver (write() syscall, manual byte swap)
│   │   │   └── disp_driver.h
│   │   ├── ipc/
│   │   │   ├── ipc_server.c        ← Unix socket server, non-blocking, 4 clients
│   │   │   ├── ipc_server.h
│   │   │   ├── cmd_parser.c        ← JSON command parser (currently only set_state)
│   │   │   └── cmd_parser.h
│   │   └── screens/
│   │       ├── scr_agent.c          ← All 5 agent states, tick-driven animations
│   │       └── scr_agent.h
│   └── lib/
│       └── lvgl/                    ← LVGL v9.2 (git submodule)
│
├── client/                          ← Desktop client tools
└── luckfox_audio_receiver/          ← ESP32-C3 firmware
```

---

## 15. Troubleshooting

| Symptom | Fix |
|---------|-----|
| Display stays blank/red after boot | SPI not working — check `/dev/spidev0.0` exists; must use `write()` not `ioctl` |
| Blue flash then black screen | LVGL render issue — ensure `lv_init()` before `disp_driver_init()` |
| Buttons not responding | Check GPIO export: `ls /sys/class/gpio/gpio57`; direction must be `in` |
| False button trigger on startup | Initialize `prev[]` from actual GPIO reads before loop |
| Text/image mirrored | Check MADCTL=`0x60`, XOFF=80, YOFF=0 |
| IPC socket not found | `luckfox_gui` failed — run in foreground, check stderr |
| `arm-rockchip830...: not found` | `export TOOLCHAIN_PATH=/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-` |
| IPC connection refused (Python → C) | `luckfox_gui` must start first; socket created after LVGL init |
| `git submodule` fails in container | Check internet: `curl -I https://github.com`; or clone LVGL on host and copy in |
| `python3` not found on board | `opkg update && opkg install python3` |
| `/api/capture` returns error "rkipc not running" | Check `ps aux | grep rkipc`; if missing, `RkLunch.sh` may have failed — reboot |
| `/api/capture` returns "No snapshots yet" | rkipc just started — wait 10s for first snapshot; check `ls /mnt/sdcard/video0/` |
| `/api/capture` returns "Snapshot incomplete after 2s" | rkipc is writing slowly — check SD card health; try rebooting |
| Snapshots not appearing in `/mnt/sdcard/` | Check `free_size_del_min` in rkipc ini — if SD card has <50MB free, rkipc won't write |
| SD card filling up fast with JPEGs | `cleanup_snapshots()` daemon not running — check Python server started correctly |
| `/api/stream` updates very slowly | Check `snapshot_interval_ms` in `/oem/usr/share/rkipc-300w.ini` — should be 200 |
| rkipc config changes not persisting across reboot | Edit `/oem/usr/share/rkipc-300w.ini` — NOT `/userdata/rkipc.ini` (overwritten on boot) |

### Debug Commands

```bash
# Board connectivity
ssh root@192.168.1.60 "ps aux | grep -E 'luckfox_gui|http_api|rkipc'"
dmesg | tail -30

# GUI / IPC
ssh root@192.168.1.60 "killall luckfox_gui; /root/Executables/luckfox_gui"
ls -la /tmp/luckfox_gui.sock

# Camera
ssh root@192.168.1.60 "ls -lht /mnt/sdcard/video0/ | head -10"
ssh root@192.168.1.60 "df -h /mnt/sdcard"
ssh root@192.168.1.60 "grep -E 'snapshot_interval|mount_path|free_size' /oem/usr/share/rkipc-300w.ini"

# Client testing (from MacBook)
python3 client/luckfox_remote.py camera-status
python3 client/luckfox_remote.py capture -o /tmp/frame.jpg && open /tmp/frame.jpg
```
