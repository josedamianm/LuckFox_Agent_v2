# LuckFox Agent V2 — Cross-Compile & Deploy Guide

This guide covers the full workflow: cloning the repo inside the Docker cross-compilation container, building the C binary, and deploying everything to the LuckFox Pico Max board.

---

## Prerequisites

- Ubuntu Server (host) with Docker installed
- Docker image `luckfox-crossdev:1.0` with the Rockchip toolchain mounted at `/toolchain`
- LuckFox Pico Max board accessible via SSH (default IP varies; check your router or use `ip addr` on the board)
- An SD card formatted and mounted at `/mnt/sdcard` on the board
- GitHub access from inside the container (either via HTTPS or SSH key)

---

## Part 1 — Cross-Compile the C Binary Inside the Docker Container

### Step 1 — Start the Docker Container

On your Ubuntu Server host, run:

```bash
docker run -it \
  --name luckfox-crossdev \
  -v ~/luckfox-crossdev/projects:/workspace \
  luckfox-crossdev:1.0 \
  /bin/bash
```

> If the container already exists and is stopped, start it with:
> ```bash
> docker start -i luckfox-crossdev
> ```

---

### Step 2 — Clone the Repository

Inside the container shell, navigate to the workspace and clone the repo:

```bash
cd /workspace
git clone https://github.com/josedamianm/LuckFox_Agent_v2.git
cd LuckFox_Agent_v2
```

---

### Step 3 — Add the LVGL Submodule

LVGL is not bundled in the repo. Pull it as a submodule:

```bash
git submodule add https://github.com/lvgl/lvgl.git lvgl_gui/lib/lvgl
cd lvgl_gui/lib/lvgl
git checkout release/v9.2
cd ../../..
```

> If you cloned a repo that already has the submodule entry in `.gitmodules`, just run:
> ```bash
> git submodule update --init --recursive
> cd lvgl_gui/lib/lvgl && git checkout release/v9.2 && cd ../../..
> ```

---

### Step 4 — Verify the Toolchain

The CMake toolchain file auto-detects the compiler in this order:
1. `TOOLCHAIN_PATH` environment variable
2. `LUCKFOX_SDK` environment variable
3. `/toolchain` (default inside the Docker container)

Inside the container the toolchain is at `/toolchain`, so no extra configuration is needed. You can verify it exists:

```bash
ls /toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-gcc
```

Expected output: the path printed back (file exists).

---

### Step 5 — Configure the Build with CMake

```bash
cd /workspace/LuckFox_Agent_v2/lvgl_gui
mkdir -p build && cd build

cmake .. \
  -DCMAKE_TOOLCHAIN_FILE=../toolchain-rv1106.cmake \
  -DCMAKE_BUILD_TYPE=Release
```

CMake should print something like:

```
-- Cross-compiling for RV1106 (ARM)
-- Toolchain: /toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-
-- Configuring done
-- Build files have been written to: .../build
```

---

### Step 6 — Compile

```bash
make -j$(nproc)
```

This compiles LVGL and all C sources and links the `luckfox_gui` binary. On success the last line will be something like:

```
[100%] Linking C executable luckfox_gui
[100%] Built target luckfox_gui
```

The binary will be at:

```
/workspace/LuckFox_Agent_v2/lvgl_gui/build/luckfox_gui
```

---

### Step 7 — Verify the Binary Architecture

```bash
file /workspace/LuckFox_Agent_v2/lvgl_gui/build/luckfox_gui
```

Expected output contains:

```
ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV)
```

---

### Step 8 — Exit the Container

```bash
exit
```

The built binary is now on your Ubuntu Server host at:

```
~/luckfox-crossdev/projects/LuckFox_Agent_v2/lvgl_gui/build/luckfox_gui
```

---

## Part 2 — Deploy to the LuckFox Pico Max Board

### Step 9 — Find the Board's IP Address

Connect the board via USB or Ethernet. On the host:

```bash
ip neighbor show | grep -i rockchip
```

Or check your router's DHCP leases. Alternatively, on the board's serial console:

```bash
ip addr show eth0
```

Replace `<BOARD_IP>` with the actual IP in all commands below (e.g., `192.168.1.100`).

---

### Step 10 — Prepare the SD Card Directory Structure

SSH into the board:

```bash
ssh root@<BOARD_IP>
```

Create the required directories on the SD card:

```bash
mkdir -p /mnt/sdcard/emoji
mkdir -p /mnt/sdcard/images
```

---

### Step 11 — Copy the C Binary to the Board

From the Ubuntu Server host (not inside the container), run:

```bash
scp ~/luckfox-crossdev/projects/LuckFox_Agent_v2/lvgl_gui/build/luckfox_gui \
    root@<BOARD_IP>:/usr/local/bin/luckfox_gui
```

Make it executable on the board:

```bash
ssh root@<BOARD_IP> "chmod +x /usr/local/bin/luckfox_gui"
```

---

### Step 12 — Copy the Python Files to the SD Card

```bash
scp ~/luckfox-crossdev/projects/LuckFox_Agent_v2/board/sdcard/gui_client.py \
    root@<BOARD_IP>:/mnt/sdcard/

scp ~/luckfox-crossdev/projects/LuckFox_Agent_v2/board/sdcard/http_api_server_v2.py \
    root@<BOARD_IP>:/mnt/sdcard/
```

---

### Step 13 — Copy Emoji Assets (Optional)

If you have PNG emoji files (240x240, named e.g. `happy.png`, `sad.png`):

```bash
scp /path/to/your/emojis/*.png root@<BOARD_IP>:/mnt/sdcard/emoji/
```

---

### Step 14 — Create the init.d Startup Script

SSH into the board and create the autostart script:

```bash
ssh root@<BOARD_IP>
```

Then on the board:

```bash
cat > /etc/init.d/S99luckfox_agent << 'EOF'
#!/bin/sh
PIDFILE_GUI=/var/run/luckfox_gui.pid
PIDFILE_API=/var/run/luckfox_api.pid

start() {
    echo "Starting luckfox_gui..."
    /usr/local/bin/luckfox_gui &
    echo $! > $PIDFILE_GUI
    sleep 2
    echo "Starting HTTP API server..."
    python3 /mnt/sdcard/http_api_server_v2.py &
    echo $! > $PIDFILE_API
}

stop() {
    echo "Stopping luckfox_agent..."
    [ -f $PIDFILE_API ] && kill $(cat $PIDFILE_API) && rm -f $PIDFILE_API
    [ -f $PIDFILE_GUI ] && kill $(cat $PIDFILE_GUI) && rm -f $PIDFILE_GUI
}

case "$1" in
    start) start ;;
    stop)  stop  ;;
    restart) stop; sleep 1; start ;;
    *) echo "Usage: $0 {start|stop|restart}"; exit 1 ;;
esac
EOF

chmod +x /etc/init.d/S99luckfox_agent
```

---

### Step 15 — Start the Services

Start immediately without rebooting:

```bash
/etc/init.d/S99luckfox_agent start
```

Or reboot to test the full autostart:

```bash
reboot
```

---

## Part 3 — Verify Everything is Working

### Step 16 — Check Running Processes

After the board boots (or after running start), verify both processes are running:

```bash
ps aux | grep -E "luckfox_gui|http_api"
```

You should see both `luckfox_gui` and `http_api_server_v2.py` in the output.

---

### Step 17 — Check the IPC Socket

```bash
ls -la /tmp/luckfox_gui.sock
```

The socket file should exist once `luckfox_gui` is running.

---

### Step 18 — Test the HTTP API

From any machine on the same network, send a test command (replace `<BOARD_IP>`):

Check status and IP:
```bash
curl http://<BOARD_IP>:8080/api/status
```

Switch to the status screen:
```bash
curl http://<BOARD_IP>:8080/api/mode/status
```

Show the eyes animation:
```bash
curl http://<BOARD_IP>:8080/api/mode/eyes
```

Show an emoji:
```bash
curl http://<BOARD_IP>:8080/api/emoji/happy
```

Show a text message:
```bash
curl -X POST http://<BOARD_IP>:8080/api/text \
     -H "Content-Type: application/json" \
     -d '{"text": "Hello from API", "color": "#00FF88", "scale": 3}'
```

Send a GIF (from your local machine using the client tool):
```bash
python3 client/luckfox_client.py <BOARD_IP> gif assets/gifs/your.gif
```

---

### Step 19 — Monitor Logs

If something is not working, check logs on the board:

```bash
# Check kernel messages for SPI/GPIO issues
dmesg | tail -30

# Run luckfox_gui in the foreground to see its stdout
pkill luckfox_gui
/usr/local/bin/luckfox_gui

# Run the API server in foreground in another terminal
python3 /mnt/sdcard/http_api_server_v2.py
```

---

## Part 4 — Rebuild After Code Changes

### Re-entering the Container and Rebuilding

```bash
docker start -i luckfox-crossdev
cd /workspace/LuckFox_Agent_v2

git pull

cd lvgl_gui/build
make -j$(nproc)
exit
```

Then repeat Steps 11–15 to redeploy the updated binary.

---

## Quick Reference

| Item | Value |
|---|---|
| Binary on board | `/usr/local/bin/luckfox_gui` |
| Python scripts | `/mnt/sdcard/` |
| Emoji PNGs | `/mnt/sdcard/emoji/<name>.png` |
| Image files | `/mnt/sdcard/images/<name>.png` |
| IPC socket | `/tmp/luckfox_gui.sock` |
| HTTP API port | `8080` |
| init.d script | `/etc/init.d/S99luckfox_agent` |
| Build output | `lvgl_gui/build/luckfox_gui` |
| Toolchain inside container | `/toolchain` |

---

## Troubleshooting

**Display stays blank after start**
- Check SPI device exists: `ls /dev/spidev0.0`
- Check GPIO export: `ls /sys/class/gpio/`
- Run `luckfox_gui` in foreground and watch for error messages

**IPC connection refused (Python → C)**
- Make sure `luckfox_gui` started successfully first (the socket is created after LVGL init)
- Check socket: `ls /tmp/luckfox_gui.sock`

**`arm-rockchip830-linux-uclibcgnueabihf-gcc: not found` during cmake**
- Verify the toolchain path: `ls /toolchain/bin/`
- Set manually: `export TOOLCHAIN_PATH=/toolchain/bin/arm-rockchip830-linux-uclibcgnueabihf-`

**`git submodule` fails inside the container**
- Check internet access: `curl -I https://github.com`
- If GitHub is unreachable, clone LVGL separately on the host and copy it into the container's mounted volume before running cmake

**python3 not found on board**
- Install it: `opkg update && opkg install python3`
- Or check: `which python3`
