# LuckFox Agent V2 — API Reference for OpenClaw Integration

**Base URL:** `https://luckfoxpico1.aiserver.onmobilespace.com`
All HTTP endpoints run on port 8080 on the board, proxied through HTTPS by Caddy via FRP tunnel.

**RTSP Video Stream:** `rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0`
Tunneled via FRP (port 8554). Must use TCP transport.

**Important — your environment:** You are running on a Ubuntu VPS (no display, no GUI). The `luckfox_remote.py` client was built for a MacBook with `ffplay` windows — the `stream` command opens GUI video/audio windows and **will not work on your headless VPS**. For your use case, interact with the HTTP API endpoints directly using `curl`, `wget`, or Python `urllib`/`requests`. You can still use RTSP programmatically with `ffmpeg`/`ffprobe` CLI (no display needed) for tasks like recording or frame extraction.

---

## 1. Device Status

`GET /api/status`
```json
{"ip": "192.168.1.60", "has_audio": true, "agent_state": "idle"}
```
Note: The `ip` field is the board's local ethernet address (not reachable from your VPS). Always use the base URL `https://luckfoxpico1.aiserver.onmobilespace.com` to reach the board.

---

## 2. Agent State Machine (Display Control)

The board has a 240x240 SPI display. The C binary (`luckfox_gui`) renders a different screen for each state. Python drives transitions via IPC over a Unix socket. You control it through the HTTP API.

5 states: `idle`, `listening`, `thinking`, `speaking`, `error`

State flow:
```
IDLE --[CTRL press]---> LISTENING --[CTRL release]---> THINKING
                                                          |
                                              [response ready]
                                                          v
IDLE <--[playback done]-- SPEAKING <--[LLM done]-- THINKING
                                                          |
IDLE <---------------- ERROR <--------[any failure]------'
```

**Read current state:**
`GET /api/agent/state`
```json
{"state": "idle"}
```

**Set state:**
`POST /api/agent/state`
Content-Type: `application/json`
```json
{"state": "speaking", "text": "Hello, I'm responding!"}
```
- `text` is optional — used for `speaking` (shows response on screen, cyan) and `error` (shows error message, red)
- Valid states: `idle`, `listening`, `thinking`, `speaking`, `error`

---

## 3. Camera

**Check camera health:**
`GET /api/camera/status`
```json
{"rkipc_running": true, "rtsp_url": "rtsp://127.0.0.1/live/0"}
```
Note: the `rtsp_url` in the response is the board's localhost address (used internally by ffmpeg on the board). From your VPS, use the public RTSP URL: `rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0`

**Capture a single JPEG frame:**
`GET /api/capture`
- Returns `image/jpeg` binary data directly
- Takes ~10 seconds (software H.265 keyframe decode on the ARM chip)
- **Set your HTTP timeout to at least 20-30 seconds**
- Internally runs: `ffmpeg -rtsp_transport tcp -i rtsp://127.0.0.1/live/0 -vf select=key -frames:v 1 -f image2 -update 1 /tmp/capture.jpg -y`

**Live RTSP video stream (direct, not through HTTP API):**
```
rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0
```
- H.265/HEVC, 2304x1296, 25 FPS, with PCM A-law 8kHz audio track
- Must use TCP transport (UDP won't work through the FRP tunnel)
- ~15 second GOP (keyframe interval) — clients joining mid-stream get decoder errors until next IDR keyframe
- On your VPS you can extract frames with: `ffmpeg -rtsp_transport tcp -i rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0 -frames:v 1 -f image2 frame.jpg`

---

## 4. Microphone — Recording Mode

The board has an INMP441 I2S MEMS microphone connected through an ESP32-C3 bridge over UART2. Audio format: **16kHz, 16-bit, mono PCM**.

The mic hardware must be explicitly started/stopped. The record API handles this automatically.

**Start recording:**
`GET /api/audio/record/start`
```json
{"recording": true}
```
Side effects: activates mic hardware (sends `AUDIO_START` to ESP32-C3), sets agent display to `listening`.

**Stop recording:**
`GET /api/audio/record/stop`
```json
{"recording": false, "bytes_captured": 64000, "wav_size": 64044}
```
Side effects: deactivates mic hardware (sends `AUDIO_STOP`), sets agent display to `thinking`.

**Download the last recording as WAV:**
`GET /api/audio/record/download`
- Returns `audio/wav` binary (16kHz, 16-bit, mono — standard WAV header + PCM)
- Returns HTTP 404 if no recording is available (WAV size <= 44 bytes = header only)

**Check recording status:**
`GET /api/audio/record_status`
```json
{"recording": false, "bytes_captured": 0, "sample_rate": 16000}
```

---

## 5. Microphone — Live Streaming Mode

`GET /api/audio/stream`
- Returns a **long-lived HTTP response** with raw PCM audio streaming continuously
- Format: signed 16-bit little-endian (`s16le`), 16kHz, mono
- Response headers include: `X-Audio-Format: s16le`, `X-Audio-Rate: 16000`, `X-Audio-Channels: 1`
- Mic hardware is automatically started when first stream client connects, stopped when last disconnects (reference counted)
- Data is batched to ~3200 bytes per write (~100ms of audio) to reduce HTTP overhead
- Multiple simultaneous stream clients are supported (fan-out via per-client queues)
- On your VPS you can pipe this to ffmpeg: `curl -N https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/stream | ffmpeg -f s16le -ar 16000 -ac 1 -i pipe:0 output.wav`

---

## 6. Speaker — Audio Playback

Audio is played through a MAX98357A I2S amplifier connected to the ESP32-C3. The Python server receives WAV data over HTTP, then streams it over UART2 as binary packets to the ESP32-C3.

**Play a WAV file on the board's speaker:**
`POST /api/audio/play`
- Content-Type: `application/octet-stream`
- Body: raw WAV file bytes (complete file including header)
- The WAV is saved to `/tmp/audio_upload.wav` on the board, then streamed via UART packets to the ESP32-C3
```json
{"audio": "playing", "size": 32044}
```
- Playback is **async** — returns immediately, audio plays in background thread
- UART bandwidth: ~921600 baud (~92KB/s). 16kHz mono 16-bit = 32KB/s, fits comfortably
- Playback is paced in real-time (sender sleeps to match audio sample rate)

**Play a test tone:**
`GET /api/audio/tone` — plays 440Hz for 2 seconds
`POST /api/audio/tone` with body `{"freq": 880, "duration": 3}` — custom frequency/duration

**Stop audio playback:**
`GET /api/audio/stop`

---

## 7. Typical AI Agent Pipeline

This is the intended end-to-end flow for a voice interaction:

```
1.  POST /api/agent/state  {"state": "listening"}     <- display shows "Listening..." with green waveform
2.  GET  /api/audio/record/start                       <- mic hardware on, starts buffering PCM
3.  (wait for user to finish speaking — use a timer, VAD, or external signal)
4.  GET  /api/audio/record/stop                        <- mic off, display shows "Thinking..." with spinner
5.  GET  /api/audio/record/download                    <- download WAV (16kHz/16-bit/mono)
6.  -> Send WAV to your STT service -> get transcript
7.  -> Send transcript to LLM -> get response text
8.  -> Send response text to TTS -> get response WAV
9.  POST /api/agent/state  {"state": "speaking", "text": "The response text"}
                                                        <- display shows "Speaking..." with response text in cyan
10. POST /api/audio/play   (body: TTS WAV bytes)       <- speaker plays response
11. (estimate playback duration: wav_bytes / (16000 * 2) seconds, then wait)
12. POST /api/agent/state  {"state": "idle"}            <- display returns to idle screen
```

**On any error:**
```
POST /api/agent/state  {"state": "error", "text": "Pipeline failed: <reason>"}
(wait 3-5 seconds)
POST /api/agent/state  {"state": "idle"}
```

**Note:** The board's physical CTRL button also triggers recording locally (pressed -> listening + mic on, released -> thinking + mic off + WAV captured), but currently the captured WAV is only logged — there is no on-board AI pipeline. All STT/LLM/TTS processing must happen externally through these APIs.

---

## 8. Important Notes & Constraints

- **Board hardware:** LuckFox Pico Max — Rockchip RV1106 ARM Cortex-A7, 256MB RAM. It's a tiny embedded board, not a server. Keep requests simple.
- **No playback completion callback.** You must estimate duration from the WAV: `duration_seconds = wav_size_bytes / (sample_rate * bytes_per_sample * channels)` = `wav_size / (16000 * 2 * 1)` = `wav_size / 32000`.
- **`/api/capture` is slow (~10s).** For anything real-time, use the RTSP stream or `/api/audio/stream` directly.
- **Mic reference counting:** The record and stream APIs share the mic hardware. Starting a recording while a stream is active (or vice versa) works fine — the ESP32-C3 only gets one `AUDIO_START` and one `AUDIO_STOP` regardless of how many consumers are active.
- **No `/api/stream` for video.** That endpoint was removed. Video is RTSP-only.
- **TTS WAV format:** The ESP32-C3 speaker driver accepts any standard WAV. The `stream_wav()` function reads the WAV header to get sample rate, bit depth, and channels, then paces the UART transmission accordingly. 16kHz/16-bit/mono is the native mic format and works well for TTS output too.

---

## 9. Reference Code

For implementation reference, here are the three key Python files running on the board and the client:

### `http_api_server_v2.py` — HTTP API Server

The main server handling all endpoints listed above. Uses `ThreadingHTTPServer` so multiple requests run concurrently. Key design points:

- Agent state is controlled via `GUIClient` which sends JSON commands over a Unix socket to the C binary (`luckfox_gui`)
- The `/api/audio/stream` endpoint keeps the HTTP connection open and streams raw PCM from a per-client queue
- Audio playback (`/api/audio/play`) saves the uploaded WAV to `/tmp/audio_upload.wav` then streams it to ESP32-C3 in a background thread
- Mic reference counting (`mic_acquire`/`mic_release`) ensures the ESP32-C3 only gets one START/STOP regardless of how many consumers are active

```python
#!/usr/bin/env python3
import json
import threading
import time
import signal
import sys
import os
import stat
import socket
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from socketserver import ThreadingMixIn

class ThreadingHTTPServer(ThreadingMixIn, HTTPServer):
    daemon_threads = True
from urllib.parse import urlparse
from gui_client import GUIClient

try:
    import audio_sender
    from audio_sender import MicReceiver
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT   = 8080
IFACE      = "eth0"
RKIPC_SOCKET = "/var/tmp/rkipc"
RTSP_URL   = "rtsp://127.0.0.1/live/0"
FFMPEG     = "/mnt/sdcard/ffmpeg"

gui = None
mic_receiver = None

# -- Mic reference counting --
# Multiple consumers (CTRL button, HTTP record, HTTP stream) can request
# the mic simultaneously.  We only send PKT_AUDIO_START on 0->1 and
# PKT_AUDIO_STOP on 1->0 so the ESP32 isn't reset mid-stream.
_mic_users = 0
_mic_lock = threading.Lock()


def mic_acquire():
    """Increment mic user count; sends AUDIO_START only on first user."""
    global _mic_users
    with _mic_lock:
        _mic_users += 1
        if _mic_users == 1:
            print("[mic] AUDIO_START (first user)")
            audio_sender.send_audio_start(16000, 16, 1)
        else:
            print(f"[mic] acquire (users={_mic_users}, already active)")


def mic_release():
    """Decrement mic user count; sends AUDIO_STOP only when last user leaves."""
    global _mic_users
    with _mic_lock:
        _mic_users = max(0, _mic_users - 1)
        if _mic_users == 0:
            print("[mic] AUDIO_STOP (last user)")
            audio_sender.send_audio_stop()
        else:
            print(f"[mic] release (users={_mic_users}, still active)")


def get_ipv4(iface):
    try:
        result = subprocess.run(
            ["ip", "-4", "addr", "show", iface],
            capture_output=True, text=True, timeout=2
        )
        for line in result.stdout.split('\n'):
            if 'inet ' in line:
                return line.split()[1].split('/')[0]
    except Exception:
        pass
    return "---"


def rkipc_running():
    try:
        return os.path.exists(RKIPC_SOCKET) and stat.S_ISSOCK(os.stat(RKIPC_SOCKET).st_mode)
    except Exception:
        return False


def capture_frame():
    if not rkipc_running():
        return None, "rkipc not running"
    try:
        tmp = '/tmp/capture.jpg'
        subprocess.run(
            [FFMPEG, '-rtsp_transport', 'tcp', '-i', RTSP_URL,
             '-vf', 'select=key',
             '-frames:v', '1', '-f', 'image2', '-update', '1', tmp, '-y'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=20
        )
        if os.path.exists(tmp):
            with open(tmp, 'rb') as f:
                data = f.read()
            os.remove(tmp)
            return data, None
        return None, "ffmpeg failed to produce a frame"
    except Exception as e:
        return None, str(e)



def on_button_event(msg):
    name = msg.get("name")
    state = msg.get("state")
    print(f"[btn] {name} {state}")

    if name == "CTRL":
        if state == "pressed":
            gui.set_state("listening")
            if mic_receiver:
                mic_receiver.start_recording()
            if HAS_AUDIO:
                mic_acquire()
        elif state == "released":
            gui.set_state("thinking")
            if HAS_AUDIO:
                mic_release()
            if mic_receiver:
                mic_receiver.stop_recording()
                wav_bytes = mic_receiver.get_wav()
                print(f"[mic] recorded {len(wav_bytes)} bytes WAV")


class APIHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def _json(self, code, data):
        self.send_response(code)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(data).encode())

    def _binary(self, code, data, content_type):
        self.send_response(code)
        self.send_header('Content-Type', content_type)
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = urlparse(self.path).path.rstrip('/')

        if path == '/api/status':
            self._json(200, {
                'ip': get_ipv4(IFACE),
                'has_audio': HAS_AUDIO,
                'agent_state': gui.state
            })

        elif path == '/api/agent/state':
            self._json(200, {'state': gui.state})

        elif path == '/api/capture':
            jpeg_data, err = capture_frame()
            if jpeg_data:
                self._binary(200, jpeg_data, 'image/jpeg')
            else:
                self._json(500, {'error': err or 'Capture failed'})

        elif path == '/api/camera/status':
            self._json(200, {
                'rkipc_running': rkipc_running(),
                'rtsp_url': RTSP_URL,
            })

        elif path == '/api/audio/record_status':
            if mic_receiver:
                with mic_receiver._lock:
                    self._json(200, {
                        'recording': mic_receiver._recording,
                        'bytes_captured': len(mic_receiver._pcm_buf),
                        'sample_rate': mic_receiver._sample_rate,
                    })
            else:
                self._json(503, {'error': 'MicReceiver not available'})

        elif path == '/api/audio/record/start':
            if not HAS_AUDIO or not mic_receiver:
                self._json(503, {'error': 'MicReceiver not available'})
            else:
                mic_receiver.start_recording()
                mic_acquire()
                gui.set_state('listening')
                self._json(200, {'recording': True})

        elif path == '/api/audio/record/stop':
            if not HAS_AUDIO or not mic_receiver:
                self._json(503, {'error': 'MicReceiver not available'})
            else:
                mic_release()
                mic_receiver.stop_recording()
                wav_bytes = mic_receiver.get_wav()
                gui.set_state('thinking')
                print(f"[mic] recorded {len(wav_bytes)} bytes WAV")
                self._json(200, {
                    'recording': False,
                    'bytes_captured': len(wav_bytes) - 44,  # minus WAV header
                    'wav_size': len(wav_bytes),
                })

        elif path == '/api/audio/record/download':
            if not mic_receiver:
                self._json(503, {'error': 'MicReceiver not available'})
            else:
                wav_bytes = mic_receiver.get_wav()
                if len(wav_bytes) <= 44:
                    self._json(404, {'error': 'No recording available'})
                else:
                    self._binary(200, wav_bytes, 'audio/wav')

        elif path == '/api/audio/stop':
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio not available'})
            else:
                try:
                    audio_sender.uart_write(audio_sender.build_packet(audio_sender.AUDIO_STOP))
                    self._json(200, {'audio': 'stopped'})
                except Exception as e:
                    self._json(500, {'error': str(e)})

        elif path == '/api/audio/tone':
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio not available'})
            else:
                def _tone():
                    audio_sender.stream_test_tone(440, 2)
                threading.Thread(target=_tone, daemon=True).start()
                self._json(200, {'audio': 'tone', 'freq': 440, 'duration': 2})

        elif path == '/api/audio/stream':
            if not HAS_AUDIO or not mic_receiver:
                self._json(503, {'error': 'Audio not available'})
                return
            # Start mic (refcounted) and create a per-client stream queue
            mic_acquire()
            stream_q = mic_receiver.add_stream()
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('X-Audio-Format', 's16le')
            self.send_header('X-Audio-Rate', '16000')
            self.send_header('X-Audio-Channels', '1')
            self.end_headers()
            print(f"[stream] client connected from {self.client_address[0]}")
            try:
                BATCH_TARGET = 3200
                batch = bytearray()
                while True:
                    try:
                        chunk = stream_q.get(timeout=0.1)
                        batch.extend(chunk)
                    except Exception:
                        pass
                    if len(batch) >= BATCH_TARGET:
                        self.wfile.write(bytes(batch))
                        self.wfile.flush()
                        batch = bytearray()
                    elif len(batch) > 0 and stream_q.empty():
                        self.wfile.write(bytes(batch))
                        self.wfile.flush()
                        batch = bytearray()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                mic_receiver.remove_stream(stream_q)
                mic_release()
                print(f"[stream] client disconnected from {self.client_address[0]}")

        else:
            self._json(404, {'error': 'Not found'})

    def do_POST(self):
        path = urlparse(self.path).path.rstrip('/')
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else b''

        if path == '/api/agent/state':
            try:
                data = json.loads(body)
                state = data.get('state', '')
                text = data.get('text')
                if state not in ('idle', 'listening', 'thinking', 'speaking', 'error'):
                    self._json(400, {'error': f'Invalid state: {state}'})
                    return
                gui.set_state(state, text)
                self._json(200, {'state': state})
            except (json.JSONDecodeError, Exception) as e:
                self._json(400, {'error': str(e)})

        elif path == '/api/audio/play':
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio module not available'})
            elif length > 0:
                tmp_path = '/tmp/audio_upload.wav'
                with open(tmp_path, 'wb') as f:
                    f.write(body)
                def _play_wav():
                    audio_sender.stream_wav(tmp_path)
                threading.Thread(target=_play_wav, daemon=True).start()
                self._json(200, {'audio': 'playing', 'size': length})
            else:
                self._json(400, {'error': 'No audio data'})

        elif path == '/api/audio/tone':
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio module not available'})
            else:
                try:
                    data = json.loads(body)
                    freq = int(data.get('freq', 440))
                    duration = int(data.get('duration', 3))
                    def _play_tone():
                        audio_sender.stream_test_tone(freq, duration)
                    threading.Thread(target=_play_tone, daemon=True).start()
                    self._json(200, {'audio': 'tone', 'freq': freq, 'duration': duration})
                except Exception as e:
                    self._json(400, {'error': str(e)})

        else:
            self._json(404, {'error': 'Not found'})


def main():
    global gui, mic_receiver

    print("LuckFox Agent V2 HTTP API Server")
    print(f"Audio support: {HAS_AUDIO}")

    if HAS_AUDIO and getattr(audio_sender, 'ser', None):
        mic_receiver = MicReceiver(audio_sender.ser)
        print("[mic] MicReceiver started")

    gui = GUIClient(event_callback=on_button_event)

    def signal_handler(sig, frame):
        print("\nShutting down...")
        gui.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    server = ThreadingHTTPServer(('0.0.0.0', API_PORT), APIHandler)
    print(f"HTTP API listening on port {API_PORT}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        gui.stop()


if __name__ == '__main__':
    main()
```

### `audio_sender.py` — UART Binary Protocol to ESP32-C3

Handles all audio I/O between the board and the ESP32-C3 over UART2 at 921600 baud. Packet format: `0xAA55` sync + type(1) + length(2) + payload + XOR checksum.

Key packet types:
- `0x01 AUDIO_START` — starts ESP32-C3 I2S (payload: sample_rate, bits, channels, total_frames)
- `0x02 AUDIO_DATA` — PCM audio chunk (512-byte payload)
- `0x03 AUDIO_STOP` — stops playback
- `0x05 PKT_MIC_DATA` — mic PCM from ESP32-C3 (received, not sent)

The `MicReceiver` class runs a background thread parsing incoming UART packets, with two parallel consumption modes:
- **Recording:** accumulates PCM into a buffer, retrieved as WAV via `get_wav()`
- **Streaming:** fan-out to per-client queues, consumed by HTTP `/api/audio/stream` handlers

```python
#!/usr/bin/env python3
"""
Audio sender for Luckfox Pico Max -> ESP32-C3 via UART2.
UART2: /dev/ttyS2
  TX = Pin 1 (GPIO1_B2_d, GPIO 42) -> ESP32-C3 GPIO 4
  RX = Pin 2 (GPIO1_B3_u, GPIO 43) <- ESP32-C3 GPIO 7
  GND = Pin 3 <-> ESP32-C3 GND
"""

import struct, time, wave, os, threading, io, queue

UART_DEV     = "/dev/ttyS2"
UART_BAUD    = 921600
CHUNK_SIZE   = 512
SYNC         = b'\xAA\x55'

AUDIO_START  = 0x01
AUDIO_DATA   = 0x02
AUDIO_STOP   = 0x03
PKT_MIC_START = 0x04
PKT_MIC_DATA  = 0x05
PKT_MIC_STOP  = 0x06
ACK          = 0x10
NACK         = 0x11
STATUS       = 0x12

try:
    import serial
    ser = serial.Serial(UART_DEV, UART_BAUD, timeout=0.1, write_timeout=0.1)
    USE_PYSERIAL = True
except ImportError:
    import termios
    fd = os.open(UART_DEV, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    attrs = termios.tcgetattr(fd)
    attrs[4] = attrs[5] = termios.B921600
    attrs[2] = termios.CS8 | termios.CLOCAL | termios.CREAD
    attrs[0] = attrs[1] = attrs[3] = 0
    termios.tcsetattr(fd, termios.TCSANOW, attrs)
    termios.tcflush(fd, termios.TCIOFLUSH)
    USE_PYSERIAL = False

def uart_write(data):
    if USE_PYSERIAL: ser.write(data)
    else: os.write(fd, data)

def uart_read(n):
    if USE_PYSERIAL: return ser.read(n)
    else:
        try: return os.read(fd, n)
        except BlockingIOError: return b''

def build_packet(pkt_type, payload=b''):
    raw = bytes([pkt_type]) + struct.pack('<H', len(payload)) + payload
    checksum = 0
    for b in raw: checksum ^= b
    return SYNC + raw + bytes([checksum])

def check_ack():
    data = uart_read(8)
    if len(data) >= 7 and data[0:2] == SYNC:
        pkt_type = data[2]
        if pkt_type in (ACK, NACK, STATUS):
            return (['ACK','NACK','STATUS'][[ACK,NACK,STATUS].index(pkt_type)], data[5])
    return None

def stream_wav(filepath):
    wf = wave.open(filepath, 'rb')
    sr, bd, ch = wf.getframerate(), wf.getsampwidth()*8, wf.getnchannels()
    total = wf.getnframes()
    print(f"Streaming: {filepath} ({sr}Hz/{bd}-bit/{ch}ch, {total} frames)")
    print(f"  UART: {UART_DEV} @ {UART_BAUD} baud")

    uart_write(build_packet(AUDIO_START, struct.pack('<HBBI', sr, bd, ch, total)))
    time.sleep(0.05)

    bytes_sent = packets = 0
    t0 = time.time()
    while True:
        chunk = wf.readframes(CHUNK_SIZE // (wf.getsampwidth() * ch))
        if not chunk: break
        uart_write(build_packet(AUDIO_DATA, chunk))
        bytes_sent += len(chunk); packets += 1

        if packets % 50 == 0:
            ack = check_ack()
            if ack and ack[0] == 'NACK':
                print("  ESP32 NACK, pausing..."); time.sleep(0.1)

        elapsed = time.time() - t0
        expected = bytes_sent / (sr * (bd // 8) * ch)
        if elapsed < expected: time.sleep(expected - elapsed)

    uart_write(build_packet(AUDIO_STOP))
    wf.close()
    print(f"  Done: {bytes_sent} bytes, {packets} pkts, {time.time()-t0:.1f}s")

def stream_test_tone(freq=440, duration=3, sample_rate=16000):
    import math
    print(f"Generating {freq}Hz tone for {duration}s at {sample_rate}Hz")
    uart_write(build_packet(AUDIO_START, struct.pack('<HBBI', sample_rate, 16, 1, sample_rate*duration)))
    time.sleep(0.05)
    t0 = time.time()
    for i in range(0, sample_rate * duration, CHUNK_SIZE // 2):
        samples = []
        for j in range(CHUNK_SIZE // 2):
            if i + j < sample_rate * duration:
                t = (i + j) / sample_rate
                samples.append(struct.pack('<h', int(16000 * math.sin(2 * math.pi * freq * t))))
        uart_write(build_packet(AUDIO_DATA, b''.join(samples)))
        elapsed = time.time() - t0
        expected = (i + len(samples)) / sample_rate
        if elapsed < expected: time.sleep(expected - elapsed)
    uart_write(build_packet(AUDIO_STOP))
    print("  Tone complete")

def send_audio_start(sample_rate=16000, bits=16, channels=1):
    uart_write(build_packet(AUDIO_START, struct.pack('<HBBI', sample_rate, bits, channels, 0)))

def send_audio_stop():
    uart_write(build_packet(AUDIO_STOP))


class AudioSender:
    """Thin wrapper exposing self.ser for MicReceiver to share the serial port."""
    def __init__(self, port=UART_DEV, baud=UART_BAUD):
        self.ser = ser if USE_PYSERIAL else None

    def send_audio_start(self, sample_rate=16000, bits=16, channels=1):
        send_audio_start(sample_rate, bits, channels)

    def send_audio_stop(self):
        send_audio_stop()


class MicReceiver:
    """
    Background thread: reads PKT_MIC_DATA from ESP32-C3 over shared UART.
    Uses bulk reads + internal byte buffer for speed at 921600 baud.

    Two consumption modes (can run simultaneously):
      - Recording: accumulates PCM into _pcm_buf, retrieved via get_wav()
      - Streaming: fan-out to per-client queues, consumed by HTTP stream handlers
    """

    SYNC_0 = 0xAA
    SYNC_1 = 0x55

    def __init__(self, serial_port):
        self._ser = serial_port
        self._pcm_buf = bytearray()
        self._recording = False
        self._lock = threading.Lock()
        self._sample_rate = 16000
        self._rx_buf = bytearray()
        self._stream_queues = []
        self._stream_lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    def start_recording(self):
        with self._lock:
            self._pcm_buf = bytearray()
            self._recording = True

    def stop_recording(self):
        with self._lock:
            self._recording = False

    def get_wav(self):
        with self._lock:
            pcm = bytes(self._pcm_buf)
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)
            wf.setframerate(self._sample_rate)
            wf.writeframes(pcm)
        return buf.getvalue()

    def add_stream(self):
        """Create a new stream queue for an HTTP client."""
        q = queue.Queue(maxsize=200)
        with self._stream_lock:
            self._stream_queues.append(q)
        return q

    def remove_stream(self, q):
        """Remove a stream queue when client disconnects."""
        with self._stream_lock:
            if q in self._stream_queues:
                self._stream_queues.remove(q)

    def has_streams(self):
        with self._stream_lock:
            return len(self._stream_queues) > 0

    def _read_bytes(self, n):
        while len(self._rx_buf) < n:
            waiting = self._ser.in_waiting
            if waiting > 0:
                chunk = self._ser.read(min(waiting, 4096))
                if chunk:
                    self._rx_buf.extend(chunk)
            else:
                chunk = self._ser.read(1)
                if chunk:
                    self._rx_buf.extend(chunk)
        result = bytes(self._rx_buf[:n])
        self._rx_buf = self._rx_buf[n:]
        return result

    def _reader_loop(self):
        while True:
            try:
                b = self._read_bytes(1)
                if b[0] != self.SYNC_0:
                    continue
                b = self._read_bytes(1)
                if b[0] != self.SYNC_1:
                    continue
                hdr = self._read_bytes(3)
                pkt_type = hdr[0]
                pkt_len = hdr[1] | (hdr[2] << 8)
                if pkt_len > 1024:
                    continue
                body = self._read_bytes(pkt_len + 1)
                payload = body[:pkt_len]
                checksum = body[pkt_len]
                running_xor = pkt_type ^ (pkt_len & 0xFF) ^ ((pkt_len >> 8) & 0xFF)
                for byte in payload:
                    running_xor ^= byte
                if checksum != running_xor:
                    continue
                self._handle_packet(pkt_type, payload)
            except Exception:
                time.sleep(0.01)
                continue

    def _handle_packet(self, ptype, data):
        if ptype == PKT_MIC_START:
            if len(data) >= 2:
                self._sample_rate = data[0] | (data[1] << 8)
        elif ptype == PKT_MIC_DATA:
            with self._lock:
                if self._recording:
                    self._pcm_buf.extend(data)
            with self._stream_lock:
                for q in self._stream_queues:
                    try:
                        q.put_nowait(bytes(data))
                    except queue.Full:
                        pass
        elif ptype == PKT_MIC_STOP:
            with self._lock:
                self._recording = False


if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1: stream_wav(sys.argv[1])
    else: stream_test_tone(440, 3)
```

### `luckfox_remote.py` — MacBook CLI Client (Reference Implementation)

Demonstrates how to call every API endpoint. Built for macOS with `ffplay` for live streaming — **not usable on headless VPS** for stream/capture display. However, the API calling patterns (`api_get`, `api_post`, `api_get_binary`) show exactly how each endpoint is consumed.

```python
#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import urllib.request
import urllib.error

DEFAULT_HOST = "https://luckfoxpico1.aiserver.onmobilespace.com"

def api_get(base, path, timeout=5):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return json.loads(body)
        except Exception:
            return {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def api_get_binary(base, path, timeout=30):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            content_type = resp.headers.get('Content-Type', '')
            data = resp.read()
            return data, content_type, None
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            err = json.loads(body)
        except Exception:
            err = {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
        return None, None, err
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def api_post(base, path, data=None, raw=None, content_type='application/json', timeout=30):
    url = f"{base}{path}"
    if raw is not None:
        body = raw
    elif data is not None:
        body = json.dumps(data).encode()
    else:
        body = b''
    try:
        req = urllib.request.Request(url, data=body, method='POST')
        req.add_header('Content-Type', content_type)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return json.loads(body)
        except Exception:
            return {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

# ... (stream command uses ffplay GUI windows - not relevant for VPS usage)
# See full file for macOS-specific stream/capture display commands
```

### Quick curl Examples for VPS Usage

```bash
# Check device status
curl -s https://luckfoxpico1.aiserver.onmobilespace.com/api/status | jq .

# Get current agent state
curl -s https://luckfoxpico1.aiserver.onmobilespace.com/api/agent/state | jq .

# Set agent state
curl -s -X POST https://luckfoxpico1.aiserver.onmobilespace.com/api/agent/state \
  -H 'Content-Type: application/json' \
  -d '{"state": "speaking", "text": "Hello world"}'

# Capture a JPEG frame (slow, ~10s)
curl -s --max-time 30 https://luckfoxpico1.aiserver.onmobilespace.com/api/capture -o frame.jpg

# Start mic recording
curl -s https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/record/start | jq .

# Stop mic recording
curl -s https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/record/stop | jq .

# Download recorded WAV
curl -s https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/record/download -o recording.wav

# Play a WAV on the board speaker
curl -s -X POST https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/play \
  -H 'Content-Type: application/octet-stream' \
  --data-binary @response.wav

# Live mic stream -> save to WAV file (Ctrl-C to stop)
curl -N https://luckfoxpico1.aiserver.onmobilespace.com/api/audio/stream | \
  ffmpeg -f s16le -ar 16000 -ac 1 -i pipe:0 output.wav

# Extract a frame from RTSP (faster alternative to /api/capture if ffmpeg is installed)
ffmpeg -rtsp_transport tcp -i rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0 \
  -frames:v 1 -f image2 frame.jpg
```
