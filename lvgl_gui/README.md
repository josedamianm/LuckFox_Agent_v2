# luckfox_gui — LVGL V2 Build & Deploy

## Prerequisites

- LuckFox Pico SDK (provides the ARM cross-compiler)
- CMake 3.16+
- Git

## 1. Add LVGL submodule

```bash
cd lvgl_gui
git submodule add https://github.com/lvgl/lvgl.git lib/lvgl
cd lib/lvgl && git checkout release/v9.2
cd ../..
```

## 2. Set SDK path

```bash
export LUCKFOX_SDK=/path/to/luckfox-pico-sdk
```

The toolchain file (`toolchain-rv1106.cmake`) expects the cross-compiler at:
```
$LUCKFOX_SDK/tools/linux/toolchain/arm-rockchip830-linux-uclibcgnueabihf/bin/
```

In the Docker container (`luckfox-crossdev`), the toolchain is mounted at `/toolchain`,
which is auto-detected when `LUCKFOX_SDK` is not set.

## 3. Build

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Output binary: `build/luckfox_gui`

## 4. Deploy to board

```bash
scp build/luckfox_gui root@<board-ip>:/root/Executables/luckfox_gui
scp board/sdcard/gui_client.py root@<board-ip>:/mnt/sdcard/
scp board/sdcard/http_api_server_v2.py root@<board-ip>:/mnt/sdcard/
```

## 5. IPC socket

The binary creates `/tmp/luckfox_gui.sock` on startup.
The Python server connects automatically via `gui_client.py`.

## Directory structure

```
lvgl_gui/
├── CMakeLists.txt
├── lv_conf.h
├── toolchain-rv1106.cmake
├── lib/
│   └── lvgl/              ← git submodule (LVGL v9.2)
└── src/
    ├── main.c
    ├── hal/
    │   ├── disp_driver.c/h
    │   └── indev_buttons.c/h
    ├── ipc/
    │   ├── ipc_server.c/h
    │   └── cmd_parser.c/h
    ├── screens/
    │   ├── scr_manager.c/h
    │   ├── scr_status.c/h
    │   ├── scr_eyes.c/h
    │   ├── scr_emoji.c/h
    │   ├── scr_text.c/h
    │   ├── scr_image.c/h
    │   └── scr_menu.c/h
    ├── anim/
    │   └── eyes_anim.c/h
    └── theme/
        └── lf_theme.c/h
```
