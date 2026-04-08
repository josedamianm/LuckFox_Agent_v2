#!/usr/bin/env python3
import json
import struct
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

API_PORT     = 8080
IFACE        = "eth0"
RKIPC_SOCKET = "/var/tmp/rkipc"
RTSP_URL     = "rtsp://127.0.0.1/live/0"
FFMPEG       = "/mnt/sdcard/ffmpeg"

gui          = None
mic_receiver = None

# ── Mic reference counting ────────────────────────────────────────────────────
# Multiple consumers (CTRL button, HTTP record, HTTP stream) can request the mic
# simultaneously. We only send PKT_AUDIO_START on 0→1 and PKT_AUDIO_STOP on 1→0.
_mic_users = 0
_mic_lock  = threading.Lock()

# ── Call state ────────────────────────────────────────────────────────────────
# A live call uses PKT_CALL_START / PKT_CALL_STOP which are separate from the
# normal AI-pipeline PKT_AUDIO_START / PKT_AUDIO_STOP. While a call is active:
#   - mic_acquire() / mic_release() are no-ops (call manages the mic lifecycle)
#   - CTRL button events are suppressed (call has priority)
_call_active = False
_call_lock   = threading.Lock()


def mic_acquire():
    """Increment mic user count; sends AUDIO_START only on first user."""
    global _mic_users
    with _mic_lock:
        if _call_active:
            print("[mic] acquire blocked: live call in progress")
            return
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
        if _call_active:
            print("[mic] release blocked: live call in progress")
            return
        _mic_users = max(0, _mic_users - 1)
        if _mic_users == 0:
            print("[mic] AUDIO_STOP (last user)")
            audio_sender.send_audio_stop()
        else:
            print(f"[mic] release (users={_mic_users}, still active)")


def call_acquire(sample_rate=8000):
    """Start a live-call session. Returns True on success, False if already active."""
    global _call_active
    with _call_lock:
        if _call_active:
            return False
        _call_active = True
    if HAS_AUDIO:
        audio_sender.start_call(sample_rate)
    print(f"[call] CALL_START @ {sample_rate} Hz")
    return True


def call_release():
    """End the live-call session."""
    global _call_active
    with _call_lock:
        if not _call_active:
            return
        _call_active = False
    if HAS_AUDIO:
        audio_sender.stop_call()
    print("[call] CALL_STOP")


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
    name  = msg.get("name")
    state = msg.get("state")
    print(f"[btn] {name} {state}")

    # Suppress CTRL button while a live call is active.
    if _call_active:
        print(f"[btn] {name} {state} suppressed — live call in progress")
        return

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

    def _stream_chunked_to_esp32(self):
        """
        Read chunked or plain HTTP POST body and forward each chunk to the ESP32
        speaker ring buffer via PKT_AUDIO_DATA packets.  Handles three encodings:
          - Transfer-Encoding: chunked
          - Content-Length: N  (read in 256-byte pieces)
          - No length header    (read until connection closes)
        """
        te = self.headers.get('Transfer-Encoding', '').lower()
        cl = self.headers.get('Content-Length', None)

        if 'chunked' in te:
            while _call_active:
                line = self.rfile.readline().strip()
                if not line:
                    break
                try:
                    size = int(line, 16)
                except ValueError:
                    break
                if size == 0:
                    break
                chunk = self.rfile.read(size)
                self.rfile.read(2)  # discard CRLF after chunk data
                if chunk and _call_active:
                    audio_sender.uart_write(
                        audio_sender.build_packet(audio_sender.AUDIO_DATA, chunk))
        elif cl:
            total    = int(cl)
            received = 0
            while received < total and _call_active:
                n     = min(256, total - received)
                chunk = self.rfile.read(n)
                if not chunk:
                    break
                received += len(chunk)
                audio_sender.uart_write(
                    audio_sender.build_packet(audio_sender.AUDIO_DATA, chunk))
        else:
            while _call_active:
                chunk = self.rfile.read(256)
                if not chunk:
                    break
                audio_sender.uart_write(
                    audio_sender.build_packet(audio_sender.AUDIO_DATA, chunk))

    def do_GET(self):
        path = urlparse(self.path).path.rstrip('/')

        if path == '/api/status':
            self._json(200, {
                'ip':          get_ipv4(IFACE),
                'has_audio':   HAS_AUDIO,
                'agent_state': gui.state,
                'call_active': _call_active,
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
                'rtsp_url':      RTSP_URL,
            })

        elif path == '/api/audio/record_status':
            if mic_receiver:
                with mic_receiver._lock:
                    self._json(200, {
                        'recording':      mic_receiver._recording,
                        'bytes_captured': len(mic_receiver._pcm_buf),
                        'sample_rate':    mic_receiver._sample_rate,
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
                    'recording':      False,
                    'bytes_captured': len(wav_bytes) - 44,
                    'wav_size':       len(wav_bytes),
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
            # ── Mic-only HTTP audio stream (AI-pipeline monitoring) ──
            if not HAS_AUDIO or not mic_receiver:
                self._json(503, {'error': 'Audio not available'})
                return
            mic_acquire()
            stream_q = mic_receiver.add_stream()
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('X-Audio-Format',   's16le')
            self.send_header('X-Audio-Rate',     str(mic_receiver._sample_rate))
            self.send_header('X-Audio-Channels', '1')
            self.end_headers()
            print(f"[stream] client connected from {self.client_address[0]}")
            try:
                BATCH_TARGET = 3200   # ~100 ms @ 16 kHz
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
                    elif batch and stream_q.empty():
                        self.wfile.write(bytes(batch))
                        self.wfile.flush()
                        batch = bytearray()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                mic_receiver.remove_stream(stream_q)
                mic_release()
                print(f"[stream] client disconnected from {self.client_address[0]}")

        elif path == '/api/audio/call/rx':
            # ── Live-call mic stream (ESP32 mic → MacBook speaker) ──
            if not HAS_AUDIO or not mic_receiver:
                self._json(503, {'error': 'Audio not available'})
                return
            if not _call_active:
                self._json(400, {'error': 'No active call — POST /api/audio/call/start first'})
                return
            stream_q = mic_receiver.add_stream()
            self.send_response(200)
            self.send_header('Content-Type',     'application/octet-stream')
            self.send_header('X-Audio-Format',   's16le')
            self.send_header('X-Audio-Rate',     str(mic_receiver._sample_rate))
            self.send_header('X-Audio-Channels', '1')
            self.end_headers()
            print(f"[call/rx] client connected from {self.client_address[0]}")
            try:
                BATCH_TARGET = 1600   # ~100 ms @ 8 kHz
                batch = bytearray()
                while _call_active:
                    try:
                        chunk = stream_q.get(timeout=0.1)
                        batch.extend(chunk)
                    except Exception:
                        pass
                    if len(batch) >= BATCH_TARGET:
                        self.wfile.write(bytes(batch))
                        self.wfile.flush()
                        batch = bytearray()
                    elif batch and stream_q.empty():
                        self.wfile.write(bytes(batch))
                        self.wfile.flush()
                        batch = bytearray()
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass
            finally:
                mic_receiver.remove_stream(stream_q)
                print(f"[call/rx] client disconnected from {self.client_address[0]}")

        elif path == '/api/audio/call/status':
            self._json(200, {
                'call_active':  _call_active,
                'sample_rate':  mic_receiver._sample_rate if mic_receiver else 8000,
            })

        else:
            self._json(404, {'error': 'Not found'})

    def do_POST(self):
        path   = urlparse(self.path).path.rstrip('/')
        length = int(self.headers.get('Content-Length', 0))
        body   = self.rfile.read(length) if length > 0 else b''

        if path == '/api/agent/state':
            try:
                data  = json.loads(body)
                state = data.get('state', '')
                text  = data.get('text')
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
                    data     = json.loads(body)
                    freq     = int(data.get('freq', 440))
                    duration = int(data.get('duration', 3))
                    def _play_tone():
                        audio_sender.stream_test_tone(freq, duration)
                    threading.Thread(target=_play_tone, daemon=True).start()
                    self._json(200, {'audio': 'tone', 'freq': freq, 'duration': duration})
                except Exception as e:
                    self._json(400, {'error': str(e)})

        elif path == '/api/audio/call/start':
            # ── Initiate live-call session ──
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio not available'})
                return
            sample_rate = 8000
            try:
                data        = json.loads(body) if body else {}
                sample_rate = int(data.get('sample_rate', 8000))
            except Exception:
                pass
            ok = call_acquire(sample_rate)
            if ok:
                if gui:
                    gui.set_state('listening', 'Live Call')
                self._json(200, {'call': 'started', 'sample_rate': sample_rate})
            else:
                self._json(409, {'error': 'Call already active'})

        elif path == '/api/audio/call/stop':
            # ── Terminate live-call session ──
            call_release()
            if gui:
                gui.set_state('idle')
            self._json(200, {'call': 'stopped'})

        elif path == '/api/audio/call/tx':
            # ── Live-call speaker stream (MacBook mic → ESP32 speaker) ──
            # Long-running POST: body is raw s16le PCM at the call sample rate.
            # Each chunk is forwarded to the ESP32 as a PKT_AUDIO_DATA packet.
            if not HAS_AUDIO:
                self._json(503, {'error': 'Audio not available'})
                return
            if not _call_active:
                self._json(400, {'error': 'No active call — POST /api/audio/call/start first'})
                return
            self.send_response(200)
            self.end_headers()
            print(f"[call/tx] client connected from {self.client_address[0]}")
            try:
                self._stream_chunked_to_esp32()
            except Exception as e:
                print(f"[call/tx] error: {e}")
            finally:
                print(f"[call/tx] client disconnected from {self.client_address[0]}")

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
        if _call_active:
            call_release()
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
