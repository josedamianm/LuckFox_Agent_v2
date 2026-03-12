#!/usr/bin/env python3
import cv2
import time
import sys
import argparse

parser = argparse.ArgumentParser(description="Luckfox RTSP Stream Test")
parser.add_argument("ip", help="Luckfox IP address (shown on LCD STATUS screen)")
args = parser.parse_args()

LUCKFOX_IP = args.ip
RTSP_MAIN = f"rtsp://{LUCKFOX_IP}:554/live/0/main_stream"
RTSP_SUB = f"rtsp://{LUCKFOX_IP}:554/live/0/sub_stream"

def test_stream(rtsp_url, stream_name):
    print(f"\n{'='*60}")
    print(f"Testing {stream_name}")
    print(f"URL: {rtsp_url}")
    print(f"{'='*60}")
    
    cap = cv2.VideoCapture(rtsp_url, cv2.CAP_FFMPEG)
    
    cap.set(cv2.CAP_PROP_BUFFERSIZE, 1)
    cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*'H264'))
    
    if not cap.isOpened():
        print(f"❌ Failed to open {stream_name}")
        return False
    
    print(f"✓ Stream opened successfully")
    
    width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS)
    
    print(f"Resolution: {width}x{height}")
    print(f"FPS: {fps}")
    
    print("\nReading frames (press 'q' to quit, 's' to switch stream)...")
    
    frame_count = 0
    start_time = time.time()
    last_fps_time = start_time
    fps_counter = 0
    
    try:
        while True:
            ret, frame = cap.read()
            
            if not ret:
                print(f"\n❌ Failed to read frame {frame_count}")
                time.sleep(0.1)
                continue
            
            frame_count += 1
            fps_counter += 1
            
            current_time = time.time()
            elapsed = current_time - last_fps_time
            
            if elapsed >= 1.0:
                actual_fps = fps_counter / elapsed
                print(f"\rFrames: {frame_count} | FPS: {actual_fps:.1f} | Size: {frame.shape}", end='')
                sys.stdout.flush()
                fps_counter = 0
                last_fps_time = current_time
            
            cv2.putText(frame, f"Frame: {frame_count}", (10, 30), 
                       cv2.FONT_HERSHEY_SIMPLEX, 1, (0, 255, 0), 2)
            cv2.putText(frame, f"{stream_name}", (10, 70), 
                       cv2.FONT_HERSHEY_SIMPLEX, 1, (255, 255, 0), 2)
            
            cv2.imshow('RTSP Stream Test', frame)
            
            key = cv2.waitKey(1) & 0xFF
            if key == ord('q'):
                print("\n\nQuitting...")
                break
            elif key == ord('s'):
                print("\n\nSwitching stream...")
                cap.release()
                cv2.destroyAllWindows()
                return True
                
    except KeyboardInterrupt:
        print("\n\nInterrupted")
    finally:
        total_time = time.time() - start_time
        avg_fps = frame_count / total_time if total_time > 0 else 0
        print(f"\n\nTotal frames: {frame_count}")
        print(f"Average FPS: {avg_fps:.2f}")
        cap.release()
        cv2.destroyAllWindows()
    
    return False

if __name__ == "__main__":
    print("Luckfox RTSP Stream Test")
    print("Commands: 'q' = quit, 's' = switch stream")
    
    streams = [
        (RTSP_SUB, "Sub Stream (704x576)"),
        (RTSP_MAIN, "Main Stream (2304x1296)")
    ]
    
    stream_idx = 0
    
    while stream_idx < len(streams):
        should_switch = test_stream(streams[stream_idx][0], streams[stream_idx][1])
        if should_switch:
            stream_idx += 1
        else:
            break
    
    print("\nTest complete")
