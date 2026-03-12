#!/usr/bin/env python3
"""
Audio sender for Luckfox Pico Max -> ESP32-C3 via UART2.
UART2: /dev/ttyS2
  TX = Pin 1 (GPIO1_B2_d, GPIO 42) -> ESP32-C3 GPIO 4
  RX = Pin 2 (GPIO1_B3_u, GPIO 43) <- ESP32-C3 GPIO 7
  GND = Pin 3 <-> ESP32-C3 GND
"""

import struct, time, wave, os

UART_DEV     = "/dev/ttyS2"
UART_BAUD    = 921600
CHUNK_SIZE   = 512
SYNC         = b'\xAA\x55'

AUDIO_START  = 0x01
AUDIO_DATA   = 0x02
AUDIO_STOP   = 0x03
ACK          = 0x10
NACK         = 0x11
STATUS       = 0x12

try:
    import serial
    ser = serial.Serial(UART_DEV, UART_BAUD, timeout=0.01, write_timeout=0.1)
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

if __name__ == '__main__':
    import sys
    if len(sys.argv) > 1: stream_wav(sys.argv[1])
    else: stream_test_tone(440, 3)
