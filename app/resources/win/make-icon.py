#!/usr/bin/env python3
"""Generate app/resources/win/app.ico (placeholder icon for T33/M4-1).

Pure-Python (no Pillow/ImageMagick needed): renders a rounded dark square with a
blue play triangle at 4x supersampling, then downsamples and writes a 32-bit
BMP ICO containing 16/32/48/256 px entries.

Run from repo root:  python3 app/resources/win/make-icon.py
"""
from __future__ import annotations

import math
import os
import struct

SIZES = (16, 32, 48, 256)
SS = 4  # supersampling factor

BG_TOP = (0x1B, 0x23, 0x33)
BG_BOTTOM = (0x0E, 0x13, 0x1C)
BORDER = (0x3A, 0x42, 0x54)
ACCENT = (0x5A, 0xA9, 0xFF)


def lerp(a, b, t):
    return a + (b - a) * t


def rounded_rect_alpha(x, y, size, radius, inset):
    """Coverage of a rounded rect [inset, size-inset]^2 (0/1, no AA at hi-res)."""
    left = inset
    top = inset
    right = size - inset
    bottom = size - inset
    if x < left or x > right or y < top or y > bottom:
        return 0.0
    # distance to nearest corner center when inside corner square
    cx = min(max(x, left + radius), right - radius)
    cy = min(max(y, top + radius), bottom - radius)
    dx = x - cx
    dy = y - cy
    if dx * dx + dy * dy <= radius * radius:
        return 1.0
    return 0.0


def triangle_contains(px, py, ax, ay, bx, by, cx, cy):
    def sign(x1, y1, x2, y2, x3, y3):
        return (x1 - x3) * (y2 - y3) - (x2 - x3) * (y1 - y3)

    d1 = sign(px, py, ax, ay, bx, by)
    d2 = sign(px, py, bx, by, cx, cy)
    d3 = sign(px, py, cx, cy, ax, ay)
    has_neg = (d1 < 0) or (d2 < 0) or (d3 < 0)
    has_pos = (d1 > 0) or (d2 > 0) or (d3 > 0)
    return not (has_neg and has_pos)


def render(size):
    hi = size * SS
    # final straight-alpha RGBA buffer at hi-res
    pixels = []
    inset = hi * 0.03
    radius = hi * 0.18
    ax, ay = hi * 0.36, hi * 0.26
    bx, by = hi * 0.36, hi * 0.74
    cx, cy = hi * 0.72, hi * 0.50
    for y in range(hi):
        row = []
        for x in range(hi):
            cov = rounded_rect_alpha(x + 0.5, y + 0.5, hi, radius, inset)
            if cov <= 0.0:
                row.append((0, 0, 0, 0.0))
                continue
            # vertical background gradient
            t = (y + 0.5) / hi
            r = lerp(BG_TOP[0], BG_BOTTOM[0], t)
            g = lerp(BG_TOP[1], BG_BOTTOM[1], t)
            b = lerp(BG_TOP[2], BG_BOTTOM[2], t)
            # 1px-ish inner border
            edge = min(x, y, hi - 1 - x, hi - 1 - y)
            if edge < hi * 0.012:
                r, g, b = BORDER
            if triangle_contains(x + 0.5, y + 0.5, ax, ay, bx, by, cx, cy):
                r, g, b = ACCENT
            row.append((int(r), int(g), int(b), cov))
        pixels.append(row)

    # Supersample downsample with premultiplied alpha to avoid dark fringes.
    out = []
    for oy in range(size):
        row = []
        for ox in range(size):
            r = g = b = a = 0.0
            for dy in range(SS):
                for dx in range(SS):
                    px = pixels[oy * SS + dy][ox * SS + dx]
                    pr, pg, pb, pa = px
                    r += pr * pa
                    g += pg * pa
                    b += pb * pa
                    a += pa
            n = SS * SS
            if a > 0:
                row.append((int(r / a + 0.5), int(g / a + 0.5), int(b / a + 0.5), int(a / n * 255 + 0.5)))
            else:
                row.append((0, 0, 0, 0))
        out.append(row)
    return out


def bmp_entry(pixels, size):
    # BITMAPINFOHEADER with doubled height, BGRA bottom-up, then AND mask (all 0).
    header = struct.pack(
        "<IiiHHIIiiII",
        40, size, size * 2, 1, 32, 0, size * size * 4 + ((size + 31) // 32) * 4 * size,
        0, 0, 0, 0,
    )
    body = bytearray()
    for y in range(size - 1, -1, -1):
        for x in range(size):
            r, g, b, a = pixels[y][x]
            body += bytes((b, g, r, a))
    mask_row_bytes = ((size + 31) // 32) * 4
    body += b"\x00" * (mask_row_bytes * size)
    return header + bytes(body)


def main():
    out_dir = os.path.dirname(os.path.abspath(__file__))
    entries = []
    for size in SIZES:
        pixels = render(size)
        entries.append((size, bmp_entry(pixels, size)))

    count = len(entries)
    data = bytearray()
    data += struct.pack("<HHH", 0, 1, count)  # ICONDIR
    offset = 6 + 16 * count
    directory = bytearray()
    for size, blob in entries:
        w = 0 if size >= 256 else size
        h = 0 if size >= 256 else size
        directory += struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(blob), offset)
        offset += len(blob)
    data += directory
    for _, blob in entries:
        data += blob
    path = os.path.join(out_dir, "app.ico")
    with open(path, "wb") as fh:
        fh.write(data)
    print(f"wrote {path} ({len(data)} bytes, sizes: {', '.join(str(s) for s in SIZES)})")


if __name__ == "__main__":
    main()
