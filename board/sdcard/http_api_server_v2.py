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
from urllib.parse import urlparse
from gui_client import GUIClient

try:
    import audio_sender
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT           = 8080
IFACE              = "eth0"
RKIPC_SOCKET       = "/var/tmp/rkipc"
RKIPC_SNAPSHOT_DIR = "/userdata"
RTSP_URL           = "rtsp://127.0.0.1/live/0"

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


def rkipc_running():
    try:
        return os.path.exists(RKIPC_SOCKET) and stat.S_ISSOCK(os.stat(RKIPC_SOCKET).st_mode)
    except Exception:
        return False


def latest_snapshot():
    newest = None
    newest_mtime = 0
    try:
        for root, dirs, files in os.walk(RKIPC_SNAPSHOT_DIR):
            for fname in files:
                if fname.endswith('.jpeg') or fname.endswith('.jpg'):
                    fpath = os.path.join(root, fname)
                    try:
                        mt = os.path.getmtime(fpath)
                        if mt > newest_mtime:
                            newest_mtime = mt
                            newest = fpath
                    except Exception:
                        pass
    except Exception:
        pass
    return newest, newest_mtime


def capture_frame():
    if not rkipc_running():
        return None, "rkipc not running"
    fpath, mtime = latest_snapshot()
    if not fpath:
        return None, "No snapshots yet — rkipc may still be initializing"
    try:
        with open(fpath, 'rb') as f:
            data = f.read()
        if not data:
            return None, "Snapshot file is empty"
        return data, None
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
            running = rkipc_running()
            fpath, mtime = latest_snapshot()
            frame_age = round(time.time() - mtime, 2) if fpath else None
            self._json(200, {
                'rkipc_running': running,
                'rtsp_url': RTSP_URL,
                'latest_snapshot': fpath,
                'snapshot_age_sec': frame_age,
                'snapshot_dir': RKIPC_SNAPSHOT_DIR
            })

        elif path == '/api/stream':
            if not rkipc_running():
                self._json(503, {'error': 'rkipc not running'})
                return
            try:
                self.send_response(200)
                self.send_header('Content-Type', 'multipart/x-mixed-replace; boundary=--luckfoxframe')
                self.send_header('Cache-Control', 'no-cache')
                self.end_headers()

                last_path = None
                while True:
                    fpath, mtime = latest_snapshot()
                    if fpath and fpath != last_path:
                        try:
                            with open(fpath, 'rb') as f:
                                data = f.read()
                            if data:
                                frame = (
                                    b'--luckfoxframe\r\n'
                                    b'Content-Type: image/jpeg\r\n'
                                    b'Content-Length: ' + str(len(data)).encode() + b'\r\n\r\n'
                                    + data + b'\r\n'
                                )
                                self.wfile.write(frame)
                                self.wfile.flush()
                                last_path = fpath
                        except Exception:
                            break
                    time.sleep(0.1)
            except (BrokenPipeError, ConnectionResetError):
                pass

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