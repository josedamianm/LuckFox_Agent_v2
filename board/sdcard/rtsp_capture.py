#!/usr/bin/env python3
"""
Simple RTSP frame capture for Luckfox (no OpenCV needed).
Uses raw RTSP/RTP to get H.265 frames, then requests JPEG via rkipc socket.
"""
import socket
import struct
import time
import os

RTSP_URL = 'rtsp://127.0.0.1:554/live/0'
RKIPC_SOCK = '/tmp/rkipc'

def request_snapshot():
    """Request snapshot via rkipc control socket."""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(2)
        sock.connect(RKIPC_SOCK)
        # Send snapshot command
        sock.send(b'snapshot\n')
        resp = sock.recv(1024)
        sock.close()
        return resp
    except Exception as e:
        print(f'rkipc error: {e}')
        return None

def rtsp_describe(host, port, path):
    """Send RTSP DESCRIBE to get stream info."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.settimeout(5)
    sock.connect((host, port))
    
    req = f'DESCRIBE rtsp://{host}:{port}{path} RTSP/1.0\r\n'
    req += f'CSeq: 1\r\n'
    req += f'Accept: application/sdp\r\n'
    req += f'\r\n'
    
    sock.send(req.encode())
    resp = sock.recv(4096)
    sock.close()
    return resp.decode('utf-8', errors='ignore')

if __name__ == '__main__':
    print('Testing RTSP connection...')
    try:
        desc = rtsp_describe('127.0.0.1', 554, '/live/0')
        print('RTSP DESCRIBE response:')
        print(desc[:500])
    except Exception as e:
        print(f'RTSP error: {e}')
    
    print('\nTesting rkipc snapshot...')
    resp = request_snapshot()
    print(f'Response: {resp}')
