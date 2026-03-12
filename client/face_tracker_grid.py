#!/usr/bin/env python3
import subprocess
import time
import sys
import argparse
import cv2
import numpy as np
from threading import Thread, Lock
from queue import Queue

parser = argparse.ArgumentParser(description="Luckfox Grid-Based Face Tracker")
parser.add_argument("ip", help="Luckfox IP address (shown on LCD STATUS screen)")
args = parser.parse_args()

LUCKFOX_IP = args.ip
RTSP_URL = f"rtsp://{LUCKFOX_IP}:554/live/0/sub_stream"
GAZE_FILE = "/tmp/gaze_zone.txt"

ZONE_NAMES = {
    0: "NO_FACE",
    1: "TOP_LEFT",
    2: "TOP_RIGHT",
    3: "BOTTOM_LEFT",
    4: "BOTTOM_RIGHT",
    5: "CENTER",
    6: "UP",
    7: "DOWN",
    8: "LEFT",
    9: "RIGHT"
}

class VideoStream:
    def __init__(self, src):
        self.stream = cv2.VideoCapture(src, cv2.CAP_FFMPEG)
        self.stream.set(cv2.CAP_PROP_BUFFERSIZE, 1)
        self.stream.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'H264'))

        self.frame = None
        self.grabbed = False
        self.stopped = False
        self.lock = Lock()

        ret, frame = self.stream.read()
        if ret:
            with self.lock:
                self.frame = frame
                self.grabbed = ret

    def start(self):
        Thread(target=self.update, daemon=True).start()
        return self

    def update(self):
        while not self.stopped:
            ret, frame = self.stream.read()

            if ret and frame is not None:
                with self.lock:
                    self.frame = frame
                    self.grabbed = ret

            time.sleep(0.001)

    def read(self):
        with self.lock:
            if self.frame is not None:
                return self.grabbed, self.frame.copy()
            else:
                return False, None

    def stop(self):
        self.stopped = True
        time.sleep(0.1)
        if self.stream.isOpened():
            self.stream.release()

def send_zone_ssh(zone):
    try:
        cmd = f"ssh root@{LUCKFOX_IP} \"echo '{zone}' > {GAZE_FILE}\""
        subprocess.run(cmd, shell=True, check=True, capture_output=True, timeout=0.5)
        return True
    except:
        return False

def get_face_zone(face_center_x, face_center_y, frame_width, frame_height):
    col_third = frame_width / 3
    row_third = frame_height / 3

    if face_center_x < col_third:
        col = 0
    elif face_center_x < 2 * col_third:
        col = 1
    else:
        col = 2

    if face_center_y < row_third:
        row = 0
    elif face_center_y < 2 * row_third:
        row = 1
    else:
        row = 2

    zone_grid = [
        [1, 6, 2],
        [8, 5, 9],
        [3, 7, 4]
    ]

    return zone_grid[row][col]

def draw_grid(frame, zone=None):
    h, w = frame.shape[:2]

    col_third = w // 3
    row_third = h // 3

    color_inactive = (80, 80, 80)
    color_active = (0, 255, 0)
    thickness = 2

    cv2.line(frame, (col_third, 0), (col_third, h), color_inactive, thickness)
    cv2.line(frame, (2 * col_third, 0), (2 * col_third, h), color_inactive, thickness)
    cv2.line(frame, (0, row_third), (w, row_third), color_inactive, thickness)
    cv2.line(frame, (0, 2 * row_third), (w, 2 * row_third), color_inactive, thickness)

    zones_coords = {
        1: (col_third // 2, row_third // 2),
        6: (col_third + col_third // 2, row_third // 2),
        2: (2 * col_third + col_third // 2, row_third // 2),
        8: (col_third // 2, row_third + row_third // 2),
        5: (col_third + col_third // 2, row_third + row_third // 2),
        9: (2 * col_third + col_third // 2, row_third + row_third // 2),
        3: (col_third // 2, 2 * row_third + row_third // 2),
        7: (col_third + col_third // 2, 2 * row_third + row_third // 2),
        4: (2 * col_third + col_third // 2, 2 * row_third + row_third // 2)
    }

    for z, (x, y) in zones_coords.items():
        color = color_active if zone == z else color_inactive
        cv2.putText(frame, str(z), (x-15, y+15),
                   cv2.FONT_HERSHEY_SIMPLEX, 1.5, color, 3)

    return frame

def face_tracking_grid():
    print(f"\nOpening RTSP stream: {RTSP_URL}")
    
    vs = VideoStream(RTSP_URL).start()
    time.sleep(1.0)
    
    grabbed, test_frame = vs.read()
    if not grabbed or test_frame is None:
        print("ERROR: Cannot open RTSP stream")
        sys.exit(1)
    
    face_cascade = cv2.CascadeClassifier(
        cv2.data.haarcascades + 'haarcascade_frontalface_default.xml'
    )
    print("✓ Using Haar Cascade with optimizations")
    
    frame_height, frame_width = test_frame.shape[:2]
    print(f"Stream resolution: {frame_width}x{frame_height}")
    
    detect_scale = 0.25
    detect_w = int(frame_width * detect_scale)
    detect_h = int(frame_height * detect_scale)
    print(f"Detection resolution: {detect_w}x{detect_h} ({detect_scale*100:.0f}%)")
    
    print("\nControls: Q=quit, G=toggle grid, F=toggle FPS, D=toggle debug")
    print("Zone mapping: 1=Top-Left, 2=Top-Right, 3=Bottom-Left, 4=Bottom-Right, 5=Center\n")
    
    current_zone = 0
    last_zone = 0
    last_face = None
    
    frame_count = 0
    detect_count = 0
    fps_start = time.time()
    actual_fps = 0
    
    show_grid = True
    show_fps = True
    show_debug = False
    
    detect_interval = 3
    
    try:
        while True:
            grabbed, frame = vs.read()
            if not grabbed or frame is None:
                continue
            
            frame_count += 1
            
            if frame_count % detect_interval == 0:
                detect_frame = cv2.resize(frame, (detect_w, detect_h), interpolation=cv2.INTER_NEAREST)
                gray = cv2.cvtColor(detect_frame, cv2.COLOR_BGR2GRAY)
                
                faces = face_cascade.detectMultiScale(
                    gray, 
                    scaleFactor=1.2,
                    minNeighbors=3,
                    minSize=(20, 20),
                    flags=cv2.CASCADE_SCALE_IMAGE
                )
                
                detect_count += 1
                
                if len(faces) > 0:
                    largest = max(faces, key=lambda f: f[2] * f[3])
                    last_face = largest
                else:
                    last_face = None
            
            if last_face is not None:
                x, y, w, h = last_face
                
                face_cx = x + w/2
                face_cy = y + h/2
                
                current_zone = get_face_zone(face_cx, face_cy, detect_w, detect_h)
                
                scale_x = frame.shape[1] / detect_w
                scale_y = frame.shape[0] / detect_h
                
                draw_x = int(x * scale_x)
                draw_y = int(y * scale_y)
                draw_w = int(w * scale_x)
                draw_h = int(h * scale_y)
                
                cv2.rectangle(frame, (draw_x, draw_y), (draw_x + draw_w, draw_y + draw_h), 
                            (0, 255, 0), 3)
                cv2.circle(frame, (int(face_cx * scale_x), int(face_cy * scale_y)), 
                          10, (0, 255, 0), -1)
            else:
                current_zone = 0
            
            if current_zone != last_zone:
                Thread(target=send_zone_ssh, args=(current_zone,), daemon=True).start()
                print(f"Zone: {current_zone} ({ZONE_NAMES[current_zone]})                    ", end='\r')
                last_zone = current_zone
            
            if show_grid:
                frame = draw_grid(frame, current_zone if current_zone > 0 else None)
            
            status_text = f"Zone: {current_zone} - {ZONE_NAMES[current_zone]}"
            cv2.putText(frame, status_text, (10, 40), 
                       cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 255), 2)
            
            elapsed = time.time() - fps_start
            if elapsed >= 1.0:
                actual_fps = frame_count / elapsed
                detection_fps = detect_count / elapsed
                frame_count = 0
                detect_count = 0
                fps_start = time.time()
            
            if show_fps and actual_fps > 0:
                cv2.putText(frame, f"Display: {actual_fps:.0f} FPS", (10, 80), 
                           cv2.FONT_HERSHEY_SIMPLEX, 0.8, (255, 255, 0), 2)
            
            if show_debug and actual_fps > 0:
                cv2.putText(frame, f"Detection: {detection_fps:.1f} FPS", (10, 115), 
                           cv2.FONT_HERSHEY_SIMPLEX, 0.7, (255, 165, 0), 2)
            
            cv2.imshow('Luckfox Grid Face Tracker', frame)
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                print("\nQuitting...")
                break
            elif key == ord('g'):
                show_grid = not show_grid
            elif key == ord('f'):
                show_fps = not show_fps
            elif key == ord('d'):
                show_debug = not show_debug
    
    except KeyboardInterrupt:
        print("\nStopped")
    finally:
        vs.stop()
        cv2.destroyAllWindows()
        send_zone_ssh(0)

if __name__ == "__main__":
    print("=" * 60)
    print("Luckfox Grid-Based Face Tracker (OPTIMIZED)")
    print("=" * 60)
    print(f"Luckfox IP: {LUCKFOX_IP}")
    print(f"RTSP URL: {RTSP_URL}")
    print()
    
    print("Testing SSH connection...")
    if not send_zone_ssh(0):
        print("ERROR: Cannot connect to Luckfox via SSH")
        sys.exit(1)
    print("✓ SSH connection OK")
    print()
    
    face_tracking_grid()
