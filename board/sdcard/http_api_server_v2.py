#!/usr/bin/env python3
import json
import threading
import time
import signal
import sys
import subprocess
from http.server import HTTPServer, BaseHTTPRequestHandler
from gui_client import GUIClient

try:
    import audio_sender
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT = 8080
IFACE = "eth0"

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


def capture_frame():
    try:
        env = {"LD_LIBRARY_PATH": "/usr/lib"}
        subprocess.run(["killall", "rkipc"], capture_output=True, timeout=3, env=env)
        time.sleep(0.3)
        result = subprocess.run(
            ["/root/Executables/get_frame"],
            capture_output=True, timeout=5, env=env
        )
        if result.returncode == 0 and result.stdout:
            return result.stdout, None
        return None, "get_frame failed"
    except subprocess.TimeoutExpired:
        return None, "Timeout"
    except Exception as e:
        return None, str(e)


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
        path = self.path.rstrip('/')

        if path == '/api/status':
            self._json(200, {'ip': get_ipv4(IFACE), 'has_audio': HAS_AUDIO})

        elif path == '/api/mode/status':
            gui.switch_screen("status")
            self._json(200, {'mode': 'status'})

        elif path == '/api/mode/eyes':
            gui.switch_screen("eyes")
            self._json(200, {'mode': 'eyes'})

        elif path.startswith('/api/emoji/'):
            name = path.split('/')[-1]
            gui.switch_screen("emoji", emoji=name)
            self._json(200, {'mode': 'emoji', 'emoji': name})

        elif path == '/api/capture':
            jpeg_data, err = capture_frame()
            if jpeg_data:
                self._binary(200, jpeg_data, 'image/jpeg')
            else:
                self._json(500, {'error': err or 'Capture failed'})

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
        path = self.path.rstrip('/')
        length = int(self.headers.get('Content-Length', 0))
        body = self.rfile.read(length) if length > 0 else b''

        if path == '/api/text':
            try:
                data = json.loads(body)
                text = data.get('text', '')
                color = data.get('color', '#FFFFFF')
                scale = int(data.get('scale', 3))
                gui.switch_screen("text", text=text, color=color, scale=scale)
                self._json(200, {'mode': 'text', 'text': text})
            except Exception as e:
                self._json(400, {'error': str(e)})

        elif path == '/api/image':
            if length > 0:
                tmp_path = '/tmp/lvgl_img.bin'
                with open(tmp_path, 'wb') as f:
                    f.write(body)
                gui.send_cmd({"cmd": "image", "path": tmp_path, "size": length})
                self._json(200, {'mode': 'image', 'size': length})
            else:
                self._json(400, {'error': 'No image data'})

        elif path == '/api/gif/frames':
            if length > 0:
                try:
                    data = json.loads(body)
                    frames = data.get('frames', [])
                    durations = data.get('durations', [])
                    gui.send_cmd({"cmd": "gif_start", "frame_count": len(frames)})
                    for i, (frame_hex, dur_ms) in enumerate(zip(frames, durations)):
                        frame_path = f'/tmp/gif_frame_{i}.bin'
                        with open(frame_path, 'wb') as f:
                            f.write(bytes.fromhex(frame_hex))
                        gui.send_cmd({
                            "cmd": "gif_frame", "index": i,
                            "path": frame_path, "duration_ms": dur_ms
                        })
                    self._json(200, {'mode': 'gif', 'frames': len(frames)})
                except Exception as e:
                    self._json(400, {'error': str(e)})
            else:
                self._json(400, {'error': 'No frame data'})

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


def on_button_event(msg):
    name = msg.get("name")
    print(f"Button event: {name}")


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
