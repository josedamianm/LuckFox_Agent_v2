# CLAUDE.md — LuckFox Agent V2

Single source of truth for AI agents working on this project.

---

## 1. Project Overview

LuckFox Agent V2 is a voice-activated AI agent running on the **LuckFox Pico Max** (Rockchip RV1106 / ARM Cortex-A7, 256MB DDR3). Two processes start at boot:

- **`luckfox_gui`** (C binary): drives LVGL v9 GUI on a 240×240 ST7789 SPI display, owns the state machine visuals, emits button events over IPC
- **`agent.py`** (Python): handles AI pipeline (STT → LLM → TTS), executes on MacBook via HTTP, drives the C binary state via IPC

```
User presses CTRL button
        ↓
luckfox_gui (C) → IPC event → agent.py (Python)
                                    ↓
                          HTTP → MacBook pipeline
                          (audio → STT → LLM → TTS)
                                    ↓
                    IPC command → luckfox_gui (C)
                                    ↓
                        LVGL → SPI → ST7789 240×240
```

---

## 2. Agent State Machine

Five states. The C binary renders the correct screen for each state. Python drives state transitions via IPC.

| State | Trigger | Visual |
|-------|---------|--------|
| `IDLE` | Boot / pipeline complete | Logo + "Press CTRL to start" |
| `LISTENING` | CTRL pressed | Mic/waveform animation, green |
| `THINKING` | CTRL released | Spinner/dots, orange |
| `SPEAKING` | LLM response ready | Response text + waveform, cyan |
| `ERROR` | Any failure | Error message, red |

### State Transitions

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

## 3. Screen Layouts (240×240, dark background)

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

## 4. IPC Protocol

Newline-delimited JSON over Unix domain socket `/tmp/luckfox_gui.sock`.

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

## 5. C Binary Structure (`lvgl_gui/src/`)

| Module | Role |
|--------|------|
| `main.c` | LVGL init, main loop, GPIO button polling, signal handling |
| `hal/disp_driver.c` | ST7789 SPI driver — init, flush_cb (byte swap) |
| `hal/disp_driver.h` | Public: `disp_driver_init()`, `disp_driver_deinit()` |
| `ipc/ipc_server.c` | Unix socket server, non-blocking, up to 4 clients |
| `ipc/ipc_server.h` | Public: `ipc_server_init()`, `ipc_server_poll()`, `ipc_broadcast()` |
| `ipc/cmd_parser.c` | Parse `set_state` JSON → call `agent_set_state()` |
| `ipc/cmd_parser.h` | Public: `cmd_parse()` |
| `screens/scr_agent.c` | All 5 agent states in one file — `agent_set_state(state, text)` |
| `screens/scr_agent.h` | Public: `agent_screen_init()`, `agent_set_state()`, `agent_tick()` |

### `main.c` Main Loop Pattern

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
- **LISTENING bars**: 7 `lv_obj` rectangles, heights oscillate with per-bar phase offset, updated every tick
- **THINKING spinner**: single `lv_arc`, start angle incremented by 4° per tick
- **SPEAKING dots**: 3 circles, opacity pulses with phase offset per dot

---

## 6. Hardware Configuration (Confirmed Working)

### Display — ST7789 240×240

| Item | Value |
|------|-------|
| SPI device | `/dev/spidev0.0` @ 32MHz, `SPI_MODE_0` |
| SPI transfer | `write()` syscall — `ioctl(SPI_IOC_MESSAGE)` silently fails on RK1106 |
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

- GPIOs must be **exported** and set to `in` direction by the binary on every startup
- Use **direct sysfs polling** in main loop (LVGL v9 keypad indev `read_cb` not polled without focused group)
- Initialize `prev[]` state from actual GPIO reads to prevent false triggers on startup
- Only CTRL button events are emitted over IPC (others reserved for future use)

### Audio — ESP32-C3 via UART2

| Item | Value |
|------|-------|
| Device | `/dev/ttyS2` |
| Baud | 921600 |
| Protocol | Binary: SYNC `0xAA55`, packet type, length, payload, XOR checksum |
| LuckFox TX | GPIO42 (Pin1) → ESP32-C3 GPIO4 |
| LuckFox RX | GPIO43 (Pin2) ← ESP32-C3 GPIO7 |

---

## 7. LVGL v9 Lessons Learned

- `lv_timer_handler()` **blocks** in partial render mode → must use `LV_DISPLAY_RENDER_MODE_FULL`
- `LV_COLOR_16_SWAP 1` is a v8 macro, **silently ignored** in v9 → manual byte swap in `flush_cb`
- `LV_COLOR_FORMAT_RGB565_SWAP` enum **does not exist** in this build → use `LV_COLOR_FORMAT_RGB565`
- LVGL v9 keypad indev requires a focused group for `read_cb` to be polled → bypass with direct GPIO loop
- Call `lv_refr_now(NULL)` after style/content changes to force immediate screen refresh
- Prefer tick-driven animations in `agent_tick()` over LVGL anim timers for predictability

---

## 8. LVGL Configuration (`lvgl_gui/lv_conf.h`)

- `LV_COLOR_DEPTH 16` (RGB565)
- `LV_COLOR_16_SWAP 0` (ignored in v9; byte swap done manually)
- Buffer: 48KB, DPI: 200, display: 240×240
- Fonts enabled: Montserrat 12, 16, 24, 32, 48

---

## 9. Build System

### Cross-compile (macOS or Ubuntu + Docker)

```bash
cd lvgl_gui
docker run --rm -v $(pwd):/work -w /work rv1106-toolchain make

# Or full cmake flow inside container
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
# Output: lvgl_gui/build/luckfox_gui (ELF 32-bit ARM)
```

Toolchain: `arm-rockchip830-linux-uclibcgnueabihf-gcc`
Flags: `-march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard -O2`
Auto-detected from `TOOLCHAIN_PATH`, `LUCKFOX_SDK`, or `/toolchain`.

### Deployment

Board IP: **192.168.1.60**, user: **root**

```bash
./sync.sh          # rsync push to board
./sync.sh pull     # pull from board
./sync.sh diff     # show differences
```

Manual:
```bash
scp lvgl_gui/build/luckfox_gui root@192.168.1.60:/root/Executables/luckfox_gui
ssh root@192.168.1.60 "killall luckfox_gui; /root/Executables/luckfox_gui"
```

### Board File Locations

| Item | Path |
|------|------|
| Binary | `/root/Executables/luckfox_gui` |
| Python scripts | `/mnt/sdcard/` |
| IPC socket | `/tmp/luckfox_gui.sock` |
| Autostart | `/etc/init.d/S99luckfox_agent` |

### Autostart Script

```sh
#!/bin/sh
start() {
    /root/Executables/luckfox_gui &
    sleep 2
    python3 /mnt/sdcard/agent.py &
}
stop() { pkill luckfox_gui; pkill -f agent.py; }
case "$1" in
    start) start ;; stop) stop ;; restart) stop; sleep 1; start ;;
esac
```

---

## 10. Current Status

`main.c` is currently a **color-test binary** — display and all 9 buttons validated:
- Boot: blue diagnostic flash → navy screen with button label instructions
- A=Red, B=Blue, X=Yellow, Y=Green, UP=Orange, DOWN=Purple, LEFT=Cyan, RIGHT=White, CTRL=Magenta

**Implementation plan** (C binary first):
1. Implement `screens/scr_agent.c` — 5 states, tick-driven animations
2. Implement `ipc/ipc_server.c` + `ipc/cmd_parser.c` — set_state commands + CTRL button events
3. Wire into `main.c` — replace color-test with agent state machine loop
4. Build, deploy, test on hardware
5. Implement Python `agent.py` — IPC client + MacBook HTTP pipeline

---

## 11. Troubleshooting

| Symptom | Fix |
|---------|-----|
| Display stays red after boot | SPI not working — check `/dev/spidev0.0`; use `write()` not `ioctl` |
| Blue flash then black screen | LVGL render issue — ensure `lv_init()` before `disp_driver_init()` |
| Buttons not responding | Check GPIO export: `ls /sys/class/gpio/gpio57`; direction must be `in` |
| False button trigger on startup | Initialize `prev[]` from actual GPIO reads before loop |
| Text/image mirrored | Check MADCTL=`0x60`, XOFF=80, YOFF=0 |
| IPC socket not found | `luckfox_gui` failed — run in foreground, check stderr |
| `arm-rockchip830...: not found` | `export TOOLCHAIN_PATH=/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-` |

Monitor:
```bash
ssh root@192.168.1.60 "killall luckfox_gui; /root/Executables/luckfox_gui"
dmesg | tail -30
```
