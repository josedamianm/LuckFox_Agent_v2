#!/usr/bin/env python3
"""
http_api_server.py — LuckFox Agent V2
HTTP API server. All display is delegated to the LVGL binary (luckfox_gui)
via Unix Domain Socket IPC. Audio handled directly via audio_sender.
"""
import json
import os
import signal
import socket
import struct
import fcntl
import subprocess
import threading
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

from gui_client import GUIClient

try:
    import audio_sender
    HAS_AUDIO = True
except ImportError:
    HAS_AUDIO = False

API_PORT  = 8080
IFACE     = "eth0"
IMG_TMP   = "/tmp/lvgl_img.bin"

_running = True
_gui     = None


def _stop(*_):
    global _running
    _running = False
    sys.exit(0)


def get_ipv4(ifname):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        ifr = struct.pack("256s", ifname[:15].encode())
        res = fcntl.ioctl(s.fileno(), 0x8915, ifr)
        return socket.inet_ntoa(res[20:24])
    except OSError:
        return None
    finally:
        s.close()


def capture_frame():
    try:
        result = subprocess.run(
            ["/root/Executables/get_frame"],
            capture_output=True, timeout=5
        )
        if result.returncode == 0 and result.stdout:
            return result.stdout, None
        return None, result.stderr.decode()
    except Exception as e:
        return None, str(e)


def on_button_event(msg):
    name   = msg.get("name", "")
    action = msg.get("action", "")
    pass


class APIHandler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _json(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _binary(self, code, data, mime):
        self.send_response(code)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        path = self.path.rstrip("/")

        if path == "/api/status":
            _gui.get_state()
            self._json(200, {
                "version": "2",
                "ip": get_ipv4(IFACE),
                "audio": HAS_AUDIO,
            })

        elif path == "/api/mode/status":
            _gui.switch_screen("status")
            self._json(200, {"mode": "status"})

        elif path == "/api/mode/eyes":
            _gui.switch_screen("eyes")
            self._json(200, {"mode": "eyes"})

        elif path.startswith("/api/emoji/"):
            name = path.split("/api/emoji/", 1)[1]
            _gui.switch_screen("emoji", emoji=name)
            self._json(200, {"mode": "emoji", "emoji": name})

        elif path == "/api/capture":
            jpeg_data, err = capture_frame()
            if jpeg_data:
                self._binary(200, jpeg_data, "image/jpeg")
            else:
                self._json(500, {"error": err or "Capture failed"})

        elif path == "/api/audio/stop":
            if not HAS_AUDIO:
                self._json(503, {"error": "Audio not available"})
            else:
                try:
                    audio_sender.uart_write(
                        audio_sender.build_packet(audio_sender.AUDIO_STOP))
                    self._json(200, {"audio": "stopped"})
                except Exception as e:
                    self._json(500, {"error": str(e)})

        elif path == "/api/audio/tone":
            if not HAS_AUDIO:
                self._json(503, {"error": "Audio not available"})
            else:
                threading.Thread(
                    target=audio_sender.stream_test_tone,
                    args=(440, 2), daemon=True).start()
                self._json(200, {"audio": "tone", "freq": 440, "duration": 2})

        else:
            self._json(404, {"error": "Not found"})

    def do_POST(self):
        path   = self.path.rstrip("/")
        length = int(self.headers.get("Content-Length", 0))
        body   = self.rfile.read(length) if length > 0 else b""

        if path == "/api/text":
            try:
                data  = json.loads(body)
                text  = data.get("text", "")
                color = data.get("color", "#FFFFFF")
                scale = int(data.get("scale", 3))
                _gui.switch_screen("text", text=text, color=color, scale=scale)
                self._json(200, {"mode": "text", "text": text,
                                 "color": color, "scale": scale})
            except Exception as e:
                self._json(400, {"error": str(e)})

        elif path == "/api/image":
            if length > 0:
                with open(IMG_TMP, "wb") as f:
                    f.write(body)
                _gui.send_image(IMG_TMP)
                self._json(200, {"mode": "image", "size": length})
            else:
                self._json(400, {"error": "No image data"})

        elif path == "/api/gif/frames":
            if length > 0:
                try:
                    data      = json.loads(body)
                    frames    = data.get("frames", [])
                    durations = data.get("durations", [])
                    if frames and durations:
                        _gui.send_cmd({"cmd": "gif_start",
                                       "frame_count": len(frames)})
                        for i, (frame_hex, dur) in enumerate(
                                zip(frames, durations)):
                            _gui.send_cmd({
                                "cmd": "gif_frame",
                                "index": i,
                                "data": frame_hex,
                                "duration_ms": dur,
                            })
                        _gui.switch_screen("gif")
                        self._json(200, {"mode": "gif",
                                         "frames": len(frames)})
                    else:
                        self._json(400, {"error": "No frames or durations"})
                except Exception as e:
                    self._json(400, {"error": str(e)})
            else:
                self._json(400, {"error": "No frame data"})

        elif path == "/api/audio/play":
            if not HAS_AUDIO:
                self._json(503, {"error": "Audio not available"})
            elif length > 0:
                tmp = "/tmp/audio_upload.wav"
                with open(tmp, "wb") as f:
                    f.write(body)
                threading.Thread(target=audio_sender.stream_wav,
                                 args=(tmp,), daemon=True).start()
                self._json(200, {"audio": "playing", "size": length})
            else:
                self._json(400, {"error": "No audio data"})

        elif path == "/api/audio/tone":
            if not HAS_AUDIO:
                self._json(503, {"error": "Audio not available"})
            else:
                try:
                    data     = json.loads(body)
                    freq     = int(data.get("freq", 440))
                    duration = int(data.get("duration", 3))
                    threading.Thread(
                        target=audio_sender.stream_test_tone,
                        args=(freq, duration), daemon=True).start()
                    self._json(200, {"audio": "tone",
                                     "freq": freq, "duration": duration})
                except Exception as e:
                    self._json(400, {"error": str(e)})

        elif path == "/api/eyes/gaze":
            try:
                data = json.loads(body)
                zone = int(data.get("zone", 0))
                _gui.set_eyes_gaze(zone)
                self._json(200, {"eyes": "gaze", "zone": zone})
            except Exception as e:
                self._json(400, {"error": str(e)})

        else:
            self._json(404, {"error": "Not found"})


def main():
    global _gui
    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    _gui = GUIClient(event_callback=on_button_event)

    server = HTTPServer(("0.0.0.0", API_PORT), APIHandler)
    server.serve_forever()


if __name__ == "__main__":
    main()
