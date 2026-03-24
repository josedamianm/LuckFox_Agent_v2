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
        self._rx_buf = bytearray()          # internal read buffer
        # Streaming: list of Queue objects, one per HTTP client
        self._stream_queues = []
        self._stream_lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()

    # ── Recording (CTRL button / record API) ────

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

    # ── Streaming (HTTP /api/audio/stream) ────

    def add_stream(self):
        """Create a new stream queue. Returns the queue for the caller to read from."""
        q = queue.Queue(maxsize=200)
        with self._stream_lock:
            self._stream_queues.append(q)
        print(f"[mic_rx] stream added (total: {len(self._stream_queues)})")
        return q

    def remove_stream(self, q):
        """Remove a stream queue when the HTTP client disconnects."""
        with self._stream_lock:
            if q in self._stream_queues:
                self._stream_queues.remove(q)
        print(f"[mic_rx] stream removed (total: {len(self._stream_queues)})")

    def has_streams(self):
        """True if any HTTP stream clients are connected."""
        with self._stream_lock:
            return len(self._stream_queues) > 0

    # ── Serial I/O ────

    def _read_bytes(self, n):
        """Read exactly n bytes from serial, using internal buffer for efficiency."""
        while len(self._rx_buf) < n:
            # Bulk read — grab everything available (up to 4096)
            waiting = self._ser.in_waiting
            if waiting > 0:
                chunk = self._ser.read(min(waiting, 4096))
                if chunk:
                    self._rx_buf.extend(chunk)
            else:
                # Nothing waiting — short sleep to avoid busy-spin
                chunk = self._ser.read(1)  # blocks up to timeout
                if chunk:
                    self._rx_buf.extend(chunk)
        result = bytes(self._rx_buf[:n])
        self._rx_buf = self._rx_buf[n:]
        return result

    def _reader_loop(self):
        """Packet parser — bulk-read version for high throughput."""
        while True:
            try:
                # ── Wait for sync bytes ────
                b = self._read_bytes(1)
                if b[0] != self.SYNC_0:
                    continue
                b = self._read_bytes(1)
                if b[0] != self.SYNC_1:
                    continue

                # ── Read header: type(1) + len(2) ────
                hdr = self._read_bytes(3)
                pkt_type = hdr[0]
                pkt_len = hdr[1] | (hdr[2] << 8)

                if pkt_len > 1024:
                    continue  # invalid, resync

                # ── Read payload + checksum in one bulk read ────
                body = self._read_bytes(pkt_len + 1)  # payload + 1 byte checksum
                payload = body[:pkt_len]
                checksum = body[pkt_len]

                # ── Verify checksum ────
                running_xor = pkt_type ^ (pkt_len & 0xFF) ^ ((pkt_len >> 8) & 0xFF)
                for byte in payload:
                    running_xor ^= byte

                if checksum != running_xor:
                    continue  # bad checksum, skip

                # ── Handle packet ────
                self._handle_packet(pkt_type, payload)

            except Exception as e:
                time.sleep(0.01)
                continue

    def _handle_packet(self, ptype, data):
        if ptype == PKT_MIC_START:
            if len(data) >= 2:
                self._sample_rate = data[0] | (data[1] << 8)
                print(f"[mic_rx] MIC_START rate={self._sample_rate}")

        elif ptype == PKT_MIC_DATA:
            # Recording: accumulate in buffer
            with self._lock:
                if self._recording:
                    self._pcm_buf.extend(data)
            # Streaming: fan-out to all connected HTTP clients
            with self._stream_lock:
                for q in self._stream_queues:
                    try:
                        q.put_nowait(bytes(data))
                    except queue.Full:
                        pass  # drop if consumer is too slow

        elif ptype == PKT_MIC_STOP:
            print("[mic_rx] MIC_STOP received")
            with self._lock:
                self._recording = False


if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1: stream_wav(sys.argv[1])
    else: stream_test_tone(440, 3)
