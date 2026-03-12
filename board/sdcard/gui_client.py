import socket
import json
import threading
import time


class GUIClient:
    SOCK_PATH = "/tmp/luckfox_gui.sock"
    RECONNECT_DELAY = 0.5

    def __init__(self, event_callback=None):
        self._sock = None
        self._lock = threading.Lock()
        self._event_cb = event_callback
        self._reader_thread = None
        self._running = True
        self._connect()

    def _connect(self):
        while self._running:
            try:
                s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                s.connect(self.SOCK_PATH)
                s.settimeout(1.0)
                with self._lock:
                    self._sock = s
                if self._reader_thread is None or not self._reader_thread.is_alive():
                    self._reader_thread = threading.Thread(
                        target=self._read_loop, daemon=True)
                    self._reader_thread.start()
                return
            except (ConnectionRefusedError, FileNotFoundError, OSError):
                time.sleep(self.RECONNECT_DELAY)

    def send_cmd(self, cmd_dict):
        msg = (json.dumps(cmd_dict) + "\n").encode()
        with self._lock:
            try:
                self._sock.sendall(msg)
            except (BrokenPipeError, OSError):
                self._sock = None
        if self._sock is None:
            self._connect()
            with self._lock:
                try:
                    self._sock.sendall(msg)
                except OSError:
                    pass

    def switch_screen(self, name, **kwargs):
        self.send_cmd({"cmd": "screen", "name": name, **kwargs})

    def set_eyes_gaze(self, zone):
        self.send_cmd({"cmd": "eyes_gaze", "zone": zone})

    def set_eyes_blink(self):
        self.send_cmd({"cmd": "eyes_blink"})

    def send_image(self, path, width=240, height=240):
        self.send_cmd({"cmd": "image", "path": path,
                       "width": width, "height": height})

    def get_state(self):
        self.send_cmd({"cmd": "get_state"})

    def stop(self):
        self._running = False
        with self._lock:
            if self._sock:
                try:
                    self._sock.close()
                except OSError:
                    pass

    def _read_loop(self):
        buf = ""
        while self._running:
            try:
                with self._lock:
                    sock = self._sock
                if sock is None:
                    time.sleep(0.1)
                    continue
                data = sock.recv(4096)
                if not data:
                    with self._lock:
                        self._sock = None
                    self._connect()
                    continue
                buf += data.decode()
                while "\n" in buf:
                    line, buf = buf.split("\n", 1)
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        msg = json.loads(line)
                        if "event" in msg and self._event_cb:
                            self._event_cb(msg)
                    except json.JSONDecodeError:
                        pass
            except socket.timeout:
                continue
            except OSError:
                with self._lock:
                    self._sock = None
                self._connect()
