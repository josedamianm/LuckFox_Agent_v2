#!/usr/bin/env python3
import sys
import argparse
from PIL import Image
import io

def convert_to_rgb565(image_path, width=240, height=240):
    img = Image.open(image_path)
    img = img.resize((width, height), Image.LANCZOS).convert('RGB')
    
    buf = bytearray(width * height * 2)
    for y in range(height):
        for x in range(width):
            r, g, b = img.getpixel((x, y))
            c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
            idx = (y * width + x) * 2
            buf[idx] = (c >> 8) & 0xFF
            buf[idx+1] = c & 0xFF
    
    return bytes(buf)

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert image to RGB565 format")
    parser.add_argument("input", help="Input image file (JPEG, PNG, etc)")
    parser.add_argument("output", help="Output raw RGB565 file")
    parser.add_argument("-w", "--width", type=int, default=240)
    parser.add_argument("-h", "--height", type=int, default=240)
    args = parser.parse_args()
    
    rgb565_data = convert_to_rgb565(args.input, args.width, args.height)
    
    with open(args.output, "wb") as f:
        f.write(rgb565_data)
    
    print(f"Converted {args.input} to {args.output} ({len(rgb565_data)} bytes)")
