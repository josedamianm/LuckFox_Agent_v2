#!/usr/bin/env python3
"""
Audio sender for Luckfox Pico Max -> ESP32-C3 via UART2.
UART2: /dev/ttyS2
  TX = Pin 1 (GPIO1_B2_d, GPIO 42) -> ESP32-C3 GPIO 4
  RX = Pin 2 (GPIO1_B3_u, GPIO 43) <- ESP32-C3 GPIO 7
  GND = Pin 3 <-> ESP32-C3 GND
"""

import struct, time, wave, os, threading, io

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
    Runs in a background thread, reads PKT_MIC_DATA packets from the ESP32-C3
    over /dev/ttyS2, and accumulates raw 16-bit PCM into a buffer.
    Call start_recording() before PKT_AUDIO_START is sent.
    Call stop_recording() after PKT_AUDIO_STOP is sent.
    Call get_wav() to retrieve the recorded audio as a WAV bytes object.
    """

    SYNC_0 = 0xAA
    SYNC_1 = 0x55

    def __init__(self, serial_port):
        """
        serial_port: the same serial.Serial instance used by audio_sender
                     (already open at 921600 baud on /dev/ttyS2)
        """
        self._ser = serial_port
        self._pcm_buf = bytearray()
        self._recording = False
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._reader_loop, daemon=True)
        self._thread.start()
        self._sample_rate = 16000   # updated when PKT_MIC_START received

    def start_recording(self):
        with self._lock:
            self._pcm_buf = bytearray()
            self._recording = True

    def stop_recording(self):
        with self._lock:
            self._recording = False

    def get_wav(self):
        """Returns a bytes object containing a valid WAV file (16-bit, mono)."""
        with self._lock:
            pcm = bytes(self._pcm_buf)
        buf = io.BytesIO()
        with wave.open(buf, 'wb') as wf:
            wf.setnchannels(1)
            wf.setsampwidth(2)          # 16-bit = 2 bytes per sample
            wf.setframerate(self._sample_rate)
            wf.writeframes(pcm)
        return buf.getvalue()

    def _reader_loop(self):
        """Packet parser — mirrors the ESP32 process_byte() state machine."""
        WAIT_SYNC0, WAIT_SYNC1, READ_TYPE, READ_LEN0, READ_LEN1, READ_PAYLOAD, READ_CHECKSUM = range(7)
        state = WAIT_SYNC0
        pkt_type = 0
        pkt_len = 0
        payload = bytearray()
        running_xor = 0
        _dbg_bytes = 0
        _dbg_pkts = 0

        while True:
            try:
                b = self._ser.read(1)
                if not b:
                    continue
                b = b[0]
                _dbg_bytes += 1
                if _dbg_bytes <= 5:
                    print(f"[mic_rx] byte {_dbg_bytes}: 0x{b:02x}")
            except Exception:
                continue

            if state == WAIT_SYNC0:
                if b == self.SYNC_0:
                    state = WAIT_SYNC1

            elif state == WAIT_SYNC1:
                state = READ_TYPE if b == self.SYNC_1 else WAIT_SYNC0

            elif state == READ_TYPE:
                pkt_type = b
                running_xor = b
                state = READ_LEN0

            elif state == READ_LEN0:
                pkt_len = b
                running_xor ^= b
                state = READ_LEN1

            elif state == READ_LEN1:
                pkt_len |= (b << 8)
                running_xor ^= b
                payload = bytearray()
                if pkt_len == 0:
                    state = READ_CHECKSUM
                elif pkt_len <= 1024:
                    state = READ_PAYLOAD
                else:
                    state = WAIT_SYNC0

            elif state == READ_PAYLOAD:
                payload.append(b)
                running_xor ^= b
                if len(payload) >= pkt_len:
                    state = READ_CHECKSUM

            elif state == READ_CHECKSUM:
                if b == running_xor:
                    _dbg_pkts += 1
                    print(f"[mic_rx] pkt #{_dbg_pkts} type=0x{pkt_type:02x} len={pkt_len}")
                    self._handle_packet(pkt_type, payload)
                else:
                    print(f"[mic_rx] checksum FAIL type=0x{pkt_type:02x} got=0x{b:02x} want=0x{running_xor:02x}")
                state = WAIT_SYNC0

    def _handle_packet(self, ptype, data):
        if ptype == PKT_MIC_START:
            if len(data) >= 2:
                self._sample_rate = data[0] | (data[1] << 8)

        elif ptype == PKT_MIC_DATA:
            with self._lock:
                if self._recording:
                    self._pcm_buf.extend(data)

        elif ptype == PKT_MIC_STOP:
            with self._lock:
                self._recording = False


if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1: stream_wav(sys.argv[1])
    else: stream_test_tone(440, 3)
