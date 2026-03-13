#!/usr/bin/env python3
import argparse
import json
import sys
import urllib.request
import urllib.error
import io

try:
    from PIL import Image
    HAS_PIL = True
except ImportError:
    HAS_PIL = False

def api_get(base, path):
    url = f"{base}{path}"
    try:
        req = urllib.request.Request(url)
        with urllib.request.urlopen(req, timeout=5) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        return json.loads(e.read())
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
            err = {'error': body.decode(errors='replace')}
        return None, None, err
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

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
        return json.loads(e.read())
    except Exception as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

def optimize_gif_to_frames(input_file, target_size=240, max_frames=30, target_fps=20):
    if not HAS_PIL:
        print("PIL not available, cannot process GIF", file=sys.stderr)
        return None, None

    try:
        print(f"Converting GIF to pre-rendered frames...", file=sys.stderr)

        with open(input_file, 'rb') as f:
            original_size = len(f.read())

        img = Image.open(input_file)

        if not getattr(img, 'is_animated', False):
            print("Not an animated GIF", file=sys.stderr)
            return None, None

        num_frames = img.n_frames
        print(f"Original: {num_frames} frames, {original_size} bytes", file=sys.stderr)

        frame_delay = int(1000 / target_fps)
        frame_step = max(1, num_frames // max_frames)

        frames_rgb565 = []
        durations = []

        for i in range(0, num_frames, frame_step):
            if len(frames_rgb565) >= max_frames:
                break

            img.seek(i)
            frame = img.convert('RGB').resize((target_size, target_size), Image.Resampling.LANCZOS)

            frame_buf = bytearray(target_size * target_size * 2)
            for y in range(target_size):
                for x in range(target_size):
                    r, g, b = frame.getpixel((x, y))
                    rgb565 = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
                    idx = (y * target_size + x) * 2
                    # Little-endian: low byte first (matches LVGL canvas format)
                    frame_buf[idx]   = rgb565 & 0xFF
                    frame_buf[idx+1] = (rgb565 >> 8) & 0xFF

            frames_rgb565.append(bytes(frame_buf))
            durations.append(frame_delay)

        print(f"Rendered: {len(frames_rgb565)} frames @ {target_fps} FPS", file=sys.stderr)

        return frames_rgb565, durations

    except Exception as e:
        print(f"Frame rendering failed: {e}", file=sys.stderr)
        return None, None

def optimize_gif(input_file, target_size=240, max_frames=30, target_fps=20):
    if not HAS_PIL:
        print("PIL not available, sending original GIF", file=sys.stderr)
        with open(input_file, 'rb') as f:
            return f.read()

    try:
        print(f"Optimizing GIF for 240x240 display...", file=sys.stderr)

        with open(input_file, 'rb') as f:
            original_size = len(f.read())

        img = Image.open(input_file)

        if not getattr(img, 'is_animated', False):
            print("Not an animated GIF, sending as-is", file=sys.stderr)
            with open(input_file, 'rb') as f:
                return f.read()

        num_frames = img.n_frames
        print(f"Original: {num_frames} frames, {original_size} bytes", file=sys.stderr)

        frame_delay = int(1000 / target_fps)

        frames_to_process = min(num_frames, max_frames)
        frame_step = max(1, num_frames // max_frames)

        optimized_frames = []

        for i in range(0, num_frames, frame_step):
            if len(optimized_frames) >= max_frames:
                break

            img.seek(i)
            frame = img.convert('RGB')
            frame = frame.resize((target_size, target_size), Image.Resampling.LANCZOS)
            optimized_frames.append(frame)

        print(f"Optimized: {len(optimized_frames)} frames @ {target_fps} FPS", file=sys.stderr)

        output = io.BytesIO()
        optimized_frames[0].save(
            output,
            format='GIF',
            save_all=True,
            append_images=optimized_frames[1:],
            duration=frame_delay,
            loop=0,
            optimize=False
        )

        optimized_data = output.getvalue()
        reduction = (1 - len(optimized_data) / original_size) * 100
        print(f"Optimized size: {len(optimized_data)} bytes ({reduction:.1f}% reduction)", file=sys.stderr)

        return optimized_data

    except Exception as e:
        print(f"Optimization failed: {e}, sending original", file=sys.stderr)
        with open(input_file, 'rb') as f:
            return f.read()

def main():
    parser = argparse.ArgumentParser(description="Luckfox Pico LCD API Client")
    parser.add_argument("ip", help="Luckfox IP address")
    parser.add_argument("--port", type=int, default=8080)
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("info", help="Get device info")
    sub.add_parser("status", help="Show status screen")
    sub.add_parser("eyes", help="Show animated eyes")

    p_emoji = sub.add_parser("emoji", help="Show emoji")
    p_emoji.add_argument("name", help="Emoji name (thumbsup, heart, smiley, star, check, xmark)")

    p_text = sub.add_parser("text", help="Display text")
    p_text.add_argument("message", help="Text to display (use \\\\n for newlines)")
    p_text.add_argument("-c", "--color", default="white",
                        help="Color: white, cyan, yellow, green, red, blue, orange, purple, pink")
    p_text.add_argument("-s", "--scale", type=int, default=3, help="Text scale (1-5)")

    p_img = sub.add_parser("image", help="Send image to display")
    p_img.add_argument("file", help="Image file path (JPEG, PNG, etc)")

    p_gif = sub.add_parser("gif", help="Send animated GIF to display")
    p_gif.add_argument("file", help="GIF file path")

    p_cap = sub.add_parser("capture", help="Capture a photo from the camera")
    p_cap.add_argument("-o", "--output", default="capture.jpg",
                       help="Output file path (default: capture.jpg)")

    p_audio = sub.add_parser("audio", help="Stream a WAV file to ESP32-C3 via UART2")
    p_audio.add_argument("file", help="WAV file path")

    p_tone = sub.add_parser("tone", help="Play a test tone on ESP32-C3 via UART2")
    p_tone.add_argument("--freq", type=int, default=440, help="Frequency in Hz (default: 440)")
    p_tone.add_argument("--duration", type=int, default=3, help="Duration in seconds (default: 3)")

    p_audio_stop = sub.add_parser("audio-stop", help="Stop audio playback on ESP32-C3")

    args = parser.parse_args()
    base = f"http://{args.ip}:{args.port}"

    if args.command == "info":
        result = api_get(base, "/api/info")
        print(json.dumps(result, indent=2))

    elif args.command == "status":
        result = api_get(base, "/api/mode/status")
        print(json.dumps(result, indent=2))

    elif args.command == "eyes":
        result = api_get(base, "/api/mode/eyes")
        print(json.dumps(result, indent=2))

    elif args.command == "emoji":
        result = api_get(base, f"/api/emoji/{args.name}")
        print(json.dumps(result, indent=2))

    elif args.command == "text":
        result = api_post(base, "/api/text", data={
            "text": args.message, "color": args.color, "scale": args.scale
        })
        print(json.dumps(result, indent=2))

    elif args.command == "image":
        print(f"Sending {args.file} to Luckfox...", file=sys.stderr)
        with open(args.file, 'rb') as f:
            image_data = f.read()
        print(f"Uploading {len(image_data)} bytes...", file=sys.stderr)
        result = api_post(base, "/api/image", raw=image_data, content_type="application/octet-stream")
        print(json.dumps(result, indent=2))

    elif args.command == "gif":
        print(f"Processing GIF {args.file}...", file=sys.stderr)
        frames, durations = optimize_gif_to_frames(args.file, target_size=240, max_frames=30, target_fps=20)

        if frames and durations:
            payload = {
                'frames': [f.hex() for f in frames],
                'durations': durations
            }
            payload_json = json.dumps(payload).encode()
            print(f"Uploading {len(frames)} pre-rendered frames ({len(payload_json)} bytes)...", file=sys.stderr)
            result = api_post(base, "/api/gif/frames", raw=payload_json, content_type="application/json", timeout=60)
            print(json.dumps(result, indent=2))
        else:
            print("Failed to process GIF, falling back to original method...", file=sys.stderr)
            gif_data = optimize_gif(args.file, target_size=240, max_frames=30, target_fps=20)
            print(f"Uploading {len(gif_data)} bytes...", file=sys.stderr)
            result = api_post(base, "/api/gif", raw=gif_data, content_type="application/octet-stream", timeout=60)
            print(json.dumps(result, indent=2))

    elif args.command == "capture":
        print("Capturing photo from Luckfox camera...", file=sys.stderr)
        data, content_type, err = api_get_binary(base, "/api/capture", timeout=30)
        if data:
            with open(args.output, "wb") as f:
                f.write(data)
            print(f"Saved {len(data)} bytes to {args.output}", file=sys.stderr)
        else:
            print(f"Capture failed: {err}", file=sys.stderr)
            sys.exit(1)

    elif args.command == "audio":
        print(f"Streaming {args.file} to ESP32-C3...", file=sys.stderr)
        with open(args.file, 'rb') as f:
            audio_data = f.read()
        print(f"Uploading {len(audio_data)} bytes...", file=sys.stderr)
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


if __name__ == "__main__":
    main()
