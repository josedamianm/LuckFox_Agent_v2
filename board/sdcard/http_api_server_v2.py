#!/usr/bin/env python3
import json
import threading
import time
import signal
import sys
import os
import socket
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from urllib.parse import urlparse
from gui_client import GUIClient

try:
    import audio_sender
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT      = 8080
IFACE         = "eth0"
CAMERA_DAEMON = "/root/Executables/camera_daemon"
MJPEG_PORT    = 8554
FRAME_LATEST  = "/tmp/frame_latest.jpg"
FRAME_TMP     = "/tmp/frame.jpg"

gui = None


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


def camera_daemon_running():
    pid_file = "/tmp/camera_daemon.pid"
    if not os.path.exists(pid_file):
        return False
    try:
        with open(pid_file) as f:
            pid = int(f.read().strip())
        os.kill(pid, 0)
        return True
    except Exception:
        return False


def capture_frame():
    if camera_daemon_running() and os.path.exists(FRAME_LATEST):
        try:
            mtime = os.path.getmtime(FRAME_LATEST)
            age   = time.time() - mtime
            if age < 5.0:
                with open(FRAME_LATEST, 'rb') as f:
                    data = f.read()
                if data:
                    return data, None
        except Exception as e:
            pass

    try:
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = "/oem/usr/lib:/usr/lib"
        result = subprocess.run(
            ["/root/Executables/get_frame", FRAME_TMP],
            capture_output=True, timeout=60, env=env
        )
        stderr = result.stderr.decode(errors='replace').strip()
        if result.returncode != 0:
            return None, f"get_frame failed (rc={result.returncode}): {stderr}"
        if not os.path.exists(FRAME_TMP) or os.path.getsize(FRAME_TMP) == 0:
            return None, f"get_frame produced empty file. stderr: {stderr}"
        with open(FRAME_TMP, 'rb') as f:
            data = f.read()
        os.remove(FRAME_TMP)
        return data, None
    except subprocess.TimeoutExpired:
        return None, "Timeout: get_frame took >60s (camera_daemon not running?)"
    except Exception as e:
        return None, str(e)


def on_button_event(msg):
    name = msg.get("name")
    state = msg.get("state")
    print(f"[btn] {name} {state}")

    if name == "A":
        if state == "pressed":
            gui.set_state("listening")
        elif state == "released":
            gui.set_state("thinking")


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
            running = camera_daemon_running()
            frame_age = None
            if os.path.exists(FRAME_LATEST):
                frame_age = round(time.time() - os.path.getmtime(FRAME_LATEST), 2)
            self._json(200, {
                'daemon_running': running,
                'frame_latest': FRAME_LATEST if os.path.exists(FRAME_LATEST) else None,
                'frame_age_sec': frame_age,
                'mjpeg_port': MJPEG_PORT
            })

        elif path == '/api/stream':
            if not camera_daemon_running():
                self._json(503, {'error': 'camera_daemon not running', 'hint': 'Start camera_daemon first'})
                return
            try:
                sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock.settimeout(3)
                sock.connect(('127.0.0.1', MJPEG_PORT))
                sock.send(b'GET / HTTP/1.0\r\nHost: localhost\r\n\r\n')

                self.send_response(200)
                self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=--luckfoxframe')
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()

                sock.settimeout(5)
                while True:
                    try:
                        chunk = sock.recv(65536)
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        self.wfile.flush()
                    except (socket.timeout, BrokenPipeError, ConnectionResetError):
                        break
                sock.close()
            except Exception as e:
                self._json(502, {'error': f'Stream proxy failed: {e}'})

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
    global gui

    print("LuckFox Agent V2 HTTP API Server")
    print(f"Audio support: {HAS_AUDIO}")

    gui = GUIClient(event_callback=on_button_event)

    def signal_handler(sig, frame):
        print("\nShutting down...")
        gui.stop()
        sys.exit(0)

    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    server = HTTPServer(('0.0.0.0', API_PORT), APIHandler)
    print(f"HTTP API listening on port {API_PORT}")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        gui.stop()


if __name__ == '__main__':
    main()