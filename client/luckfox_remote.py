#!/usr/bin/env python3
import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import urllib.request
import urllib.error

DEFAULT_HOST = "https://luckfoxpico1.aiserver.onmobilespace.com"

def api_get(base, path, timeout=5):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return json.loads(body)
        except Exception:
            return {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def api_get_binary(base, path, timeout=30):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            content_type = resp.headers.get('Content-Type', '')
            data = resp.read()
            return data, content_type, None
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            err = json.loads(body)
        except Exception:
            err = {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
        return None, None, err
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)


def capture_from_stream(base, timeout=10):
    """Connect to /api/stream, extract first complete JPEG frame, return bytes."""
    url = f"{base}/api/stream"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            buf = b''
            while True:
                chunk = resp.read(4096)
                if not chunk:
                    break
                buf += chunk
                start = buf.find(b'\xff\xd8')
                if start != -1:
                    end = buf.find(b'\xff\xd9', start)
                    if end != -1:
                        return buf[start:end + 2], None
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            err = json.loads(body)
        except Exception:
            err = {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
        return None, err
    except Exception as e:
        return None, {'error': str(e)}

def api_post(base, path, data=None, raw=None, content_type='application/json', timeout=30):
    url = f"{base}{path}"
    if raw is not None:
        body = raw
    elif data is not None:
        body = json.dumps(data).encode()
    else:
        body = b''
    try:
        req = urllib.request.Request(url, data=body, method='POST')
        req.add_header('Content-Type', content_type)
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body = e.read()
        try:
            return json.loads(body)
        except Exception:
            return {'error': f'HTTP {e.code}', 'body': body.decode(errors='replace')}
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def main():
    epilog = """
available commands & API endpoints:
  status                GET  /api/status              device info (ip, audio, agent_state)
  state                 GET  /api/agent/state          current agent state
  set <state>           POST /api/agent/state          set agent state
  capture               GET  /api/capture              grab one JPEG keyframe (~10s)
  camera-status         GET  /api/camera/status        rkipc health + RTSP URL
  stream                                               open live video+audio (RTSP + HTTP mic)
  audio <file>          POST /api/audio/play           upload and play a WAV file on the board
  tone                  POST /api/audio/tone           play a sine-wave test tone
  audio-stop            GET  /api/audio/stop           stop audio playback
  record-status         GET  /api/audio/record_status  mic recording state + bytes captured
  record-start          GET  /api/audio/record/start   start mic recording (simulates CTRL press)
  record-stop           GET  /api/audio/record/stop    stop mic recording + report WAV size
  record-download       GET  /api/audio/record/download download last recording as WAV file

stream command:
  Opens live video (RTSP from rkipc) + live mic audio (HTTP from ESP32-C3).
  Requires ffplay (from ffmpeg) installed on the client machine.
  Video passes through untouched; audio is raw PCM s16le 16kHz mono.
  The MacBook does all the heavy lifting — board just serves raw bytes.

  %(prog)s stream                        live video + audio (default)
  %(prog)s stream --audio-only           mic audio only (no video)
  %(prog)s stream --video-only           RTSP video only (no mic)
  %(prog)s stream --record out.mp4       record video+audio to file (Ctrl-C to stop)

agent states:
  idle       — waiting for input
  listening  — recording audio (CTRL held)
  thinking   — processing (CTRL released)
  speaking   — playing TTS response (--text sets display text)
  error      — error state (--text sets error message)

examples:
  %(prog)s status
  %(prog)s set idle
  %(prog)s set speaking --text "Hello!"
  %(prog)s capture -o frame.jpg
  %(prog)s stream
  %(prog)s stream --record clip.mp4
  %(prog)s tone --freq 880 --duration 2
  %(prog)s audio response.wav
  %(prog)s record-start && sleep 5 && %(prog)s record-stop
  %(prog)s record-download -o mic.wav

  %(prog)s --host 192.168.1.60 status
  %(prog)s --host 192.168.1.60 stream
"""
    parser = argparse.ArgumentParser(
        description="LuckFox Agent V2 API Client",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=epilog
    )
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"Base URL or IP (default: {DEFAULT_HOST})")
    parser.add_argument("--port", type=int, default=None,
                        help="Port override when --host is an IP (default: 8080)")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("status", help="GET /api/status — device status (ip, audio, agent_state)")
    sub.add_parser("state", help="GET /api/agent/state — current agent state")

    p_set = sub.add_parser("set", help="POST /api/agent/state — set agent state")
    p_set.add_argument("state", choices=["idle", "listening", "thinking", "speaking", "error"])
    p_set.add_argument("--text", default=None, help="Text for speaking/error states")

    p_cap = sub.add_parser("capture", help="/api/stream — grab one JPEG frame from the MJPEG stream")
    p_cap.add_argument("-o", "--output", default="capture.jpg",
                       help="Output file path (default: capture.jpg)")

    sub.add_parser("camera-status", help="GET /api/camera/status — camera daemon health + frame age")

    p_stream = sub.add_parser("stream", help="Open live video+audio stream (RTSP video + HTTP mic audio)")
    p_stream.add_argument("--record", metavar="FILE", default=None,
                          help="Record to file instead of playing (e.g. --record output.mp4)")
    p_stream.add_argument("--audio-only", action="store_true",
                          help="Stream audio only (no video)")
    p_stream.add_argument("--video-only", action="store_true",
                          help="Stream video only (no mic audio, same as RTSP)")
    p_stream.add_argument("--rtsp", default=None,
                          help="Override RTSP URL")

    p_audio = sub.add_parser("audio", help="POST /api/audio/play — upload and play a WAV file")
    p_audio.add_argument("file", help="WAV file path")

    p_tone = sub.add_parser("tone", help="POST /api/audio/tone — play a test tone")
    p_tone.add_argument("--freq", type=int, default=440, help="Frequency in Hz (default: 440)")
    p_tone.add_argument("--duration", type=int, default=3, help="Duration in seconds (default: 3)")

    sub.add_parser("audio-stop", help="GET /api/audio/stop — stop audio playback")

    sub.add_parser("record-status", help="GET /api/audio/record_status — mic state + bytes captured")
    sub.add_parser("record-start", help="GET /api/audio/record/start — start recording (simulates CTRL press)")
    sub.add_parser("record-stop", help="GET /api/audio/record/stop — stop recording + report WAV size")
    p_dl = sub.add_parser("record-download", help="GET /api/audio/record/download — download last WAV")
    p_dl.add_argument("-o", "--output", default="recording.wav", help="Output file (default: recording.wav)")

    args = parser.parse_args()

    host = args.host
    if host.startswith("http://") or host.startswith("https://"):
        base = host.rstrip("/")
    else:
        port = args.port if args.port else 8080
        base = f"http://{host}:{port}"

    if args.command == "status":
        result = api_get(base, "/api/status")
        print(json.dumps(result, indent=2))

    elif args.command == "state":
        result = api_get(base, "/api/agent/state")
        print(json.dumps(result, indent=2))

    elif args.command == "set":
        data = {"state": args.state}
        if args.text:
            data["text"] = args.text
        result = api_post(base, "/api/agent/state", data=data)
        print(json.dumps(result, indent=2))

    elif args.command == "capture":
        print("Capturing frame...", file=sys.stderr)
        data, content_type, err = api_get_binary(base, "/api/capture", timeout=30)
        if data:
            with open(args.output, "wb") as f:
                f.write(data)
            print(f"Saved {len(data)} bytes to {args.output}", file=sys.stderr)
        else:
            print(f"Capture failed: {err}", file=sys.stderr)
            sys.exit(1)

    elif args.command == "camera-status":
        result = api_get(base, "/api/camera/status")
        print(json.dumps(result, indent=2))

    elif args.command == "stream":
        # Resolve RTSP and audio stream URLs
        rtsp_url = args.rtsp or "rtsp://luckfoxpico1.aiserver.onmobilespace.com:8554/live/0"
        audio_url = f"{base}/api/audio/stream"

        # Check for ffplay / ffmpeg
        ffplay = shutil.which("ffplay")
        ffmpeg = shutil.which("ffmpeg")

        if args.video_only:
            # Simple RTSP-only playback with VideoToolbox HW decode
            if not ffplay:
                print("ffplay not found. Install ffmpeg or open in VLC:")
                print(f"  vlc {rtsp_url}")
                sys.exit(1)
            print(f"Opening video-only stream...")
            print(f"  RTSP: {rtsp_url} (VideoToolbox HW decode)")
            print("Press 'q' in the ffplay window to quit.")
            try:
                subprocess.run([
                    ffplay, "-rtsp_transport", "tcp",
                    "-hwaccel", "videotoolbox",
                    rtsp_url
                ])
            except KeyboardInterrupt:
                pass

        elif args.audio_only:
            # Audio-only from HTTP mic stream
            if not ffplay:
                print("ffplay not found. Install ffmpeg, or use curl:")
                print(f"  curl -N {audio_url} | ffplay -f s16le -ar 16000 -ch_layout mono -nodisp -")
                sys.exit(1)
            print(f"Opening audio-only stream (mic)...")
            print(f"  Audio: {audio_url}")
            print("Press 'q' or Ctrl-C to quit.")
            try:
                subprocess.run([
                    ffplay, "-nodisp",
                    "-f", "s16le", "-ar", "16000", "-ch_layout", "mono",
                    "-af", "volume=5",
                    audio_url
                ])
            except KeyboardInterrupt:
                pass

        elif args.record:
            # Record video+audio to file using ffmpeg
            if not ffmpeg:
                print("ffmpeg not found. Install ffmpeg first.", file=sys.stderr)
                sys.exit(1)
            outfile = args.record
            print(f"Recording to {outfile}...")
            print(f"  Video: {rtsp_url}")
            print(f"  Audio: {audio_url}")
            print("Press Ctrl-C to stop recording.")
            cmd = [
                ffmpeg,
                "-rtsp_transport", "tcp",
                "-hwaccel", "videotoolbox",
                "-i", rtsp_url,
                "-f", "s16le", "-ar", "16000", "-ch_layout", "mono", "-i", audio_url,
                "-map", "0:v", "-map", "1:a",
                "-c:v", "copy", "-c:a", "aac", "-b:a", "64k",
                outfile, "-y"
            ]
            try:
                subprocess.run(cmd)
            except KeyboardInterrupt:
                pass
            if os.path.exists(outfile):
                size = os.path.getsize(outfile)
                print(f"\nSaved {size} bytes to {outfile}")

        else:
            # Default: live video + audio playback
            # Open RTSP video and HTTP mic audio as two separate ffplay processes.
            # The pipe-mux approach (ffmpeg → NUT → ffplay) fails because rkipc uses
            # a very long GOP (~15s); ffmpeg copies P-frames before the first IDR into
            # the pipe and ffplay's HEVC decoder shows noise until an IDR arrives.
            # Two separate processes match how VLC handles the RTSP stream natively.
            if not ffplay:
                print("ffplay not found. Install ffmpeg, or open in VLC:")
                print(f"  vlc {rtsp_url}")
                sys.exit(1)
            print(f"Opening live stream (video + mic audio)...")
            print(f"  Video: {rtsp_url}")
            print(f"  Audio: {audio_url}")
            print("Press 'q' in the video window to quit.")
            try:
                # Video: direct RTSP → ffplay with low-latency flags to reduce RTSP buffer delay.
                # -fflags nobuffer / -flags low_delay: disable input buffering
                # -framedrop: drop late frames instead of building up lag
                # -probesize 32 / -analyzeduration 0: start decoding immediately
                video = subprocess.Popen([
                    ffplay,
                    "-rtsp_transport", "tcp",
                    "-fflags", "nobuffer",
                    "-flags", "low_delay",
                    "-framedrop",
                    "-probesize", "32",
                    "-analyzeduration", "0",
                    rtsp_url
                ], stderr=subprocess.DEVNULL)
                # Audio: HTTP mic stream → ffplay (no display)
                audio = subprocess.Popen([
                    ffplay, "-nodisp",
                    "-f", "s16le", "-ar", "16000", "-ch_layout", "mono",
                    "-af", "volume=5",
                    audio_url
                ], stderr=subprocess.DEVNULL)
                video.wait()
                audio.terminate()
            except KeyboardInterrupt:
                pass

    elif args.command == "audio":
        print(f"Uploading {args.file}...", file=sys.stderr)
        with open(args.file, 'rb') as f:
            audio_data = f.read()
        print(f"{len(audio_data)} bytes", file=sys.stderr)
        result = api_post(base, "/api/audio/play", raw=audio_data,
                          content_type="application/octet-stream", timeout=120)
        print(json.dumps(result, indent=2))

    elif args.command == "tone":
        result = api_post(base, "/api/audio/tone",
                          data={"freq": args.freq, "duration": args.duration})
        print(json.dumps(result, indent=2))

    elif args.command == "audio-stop":
        result = api_get(base, "/api/audio/stop")
        print(json.dumps(result, indent=2))

    elif args.command == "record-status":
        result = api_get(base, "/api/audio/record_status")
        print(json.dumps(result, indent=2))

    elif args.command == "record-start":
        result = api_get(base, "/api/audio/record/start")
        print(json.dumps(result, indent=2))

    elif args.command == "record-stop":
        result = api_get(base, "/api/audio/record/stop")
        print(json.dumps(result, indent=2))

    elif args.command == "record-download":
        print(f"Downloading recording...", file=sys.stderr)
        data, content_type, err = api_get_binary(base, "/api/audio/record/download", timeout=10)
        if data:
            with open(args.output, "wb") as f:
                f.write(data)
            print(f"Saved {len(data)} bytes to {args.output}", file=sys.stderr)
        else:
            print(f"Download failed: {err}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()
