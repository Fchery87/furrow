#!/usr/bin/env python3
"""Generate Furrow's Ploughed Ridge PNG, ICO, and ICNS brand icons using pure Python."""
import math
import struct
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT_DIR = ROOT / 'assets' / 'brand'

# Graphite brand colors (RGBA 0..255)
C_BG = (0x14, 0x16, 0x1B, 0xFF)
C_BORDER = (0x30, 0x35, 0x42, 0xFF)
C_INNER = (0x22, 0x26, 0x30, 0xFF)
C_RIDGE = (0xEC, 0xEE, 0xF2, 0xFF)
C_AMBER = (0xD9, 0x9B, 0x38, 0xFF)
C_CLEAR = (0, 0, 0, 0)


def in_rounded_rect(x, y, x0, y0, x1, y1, r):
    if x < x0 or x > x1 or y < y0 or y > y1:
        return False
    cx = min(max(x, x0 + r), x1 - r)
    cy = min(max(y, y0 + r), y1 - r)
    dx = x - cx
    dy = y - cy
    return dx * dx + dy * dy <= r * r


def in_trapezoid(x, y, left, top, bottom, top_right, bottom_right):
    if y < top or y > bottom or x < left:
        return False
    t = (y - top) / (bottom - top)
    right = top_right + (bottom_right - top_right) * t
    return x <= right


def sample_point(u, v, small_optical):
    """Evaluate Ploughed Ridge icon color at normalized 64x64 coordinate (u, v)."""
    # Outer chassis rounded square: [1..63], radius 14
    if not in_rounded_rect(u, v, 1.0, 1.0, 63.0, 63.0, 14.5):
        return C_CLEAR

    border_w = 2.6 if small_optical else 2.0
    if not in_rounded_rect(u, v, 1.0 + border_w, 1.0 + border_w, 63.0 - border_w, 63.0 - border_w, max(2.0, 14.5 - border_w)):
        return C_BORDER

    # Top ridge: M16 16 H48 L44 24.5 H16 Z (slightly bolder at <= 24px for crispness)
    if small_optical:
        if in_trapezoid(u, v, 14.5, 14.5, 24.0, 49.5, 45.2):
            return C_RIDGE
        if in_trapezoid(u, v, 14.5, 27.8, 36.8, 41.0, 36.8):
            return C_RIDGE
        if in_rounded_rect(u, v, 14.5, 40.5, 25.5, 49.5, 2.0):
            return C_RIDGE
        if in_rounded_rect(u, v, 38.5, 40.5, 49.5, 49.5, 2.0):
            return C_AMBER
        return C_BG

    if in_trapezoid(u, v, 16.0, 16.0, 24.5, 48.0, 44.0):
        return C_RIDGE
    if in_trapezoid(u, v, 16.0, 28.5, 36.5, 40.0, 36.0):
        return C_RIDGE
    if in_rounded_rect(u, v, 16.0, 40.5, 25.5, 48.5, 2.0):
        return C_RIDGE
    if in_rounded_rect(u, v, 38.5, 40.5, 48.0, 48.5, 2.0):
        return C_AMBER

    # Subtle inner frame stroke on icons >= 32px
    in_outer_inner = in_rounded_rect(u, v, 5.5, 5.5, 58.5, 58.5, 11.0)
    in_inner_hole = in_rounded_rect(u, v, 6.5, 6.5, 57.5, 57.5, 10.0)
    if in_outer_inner and not in_inner_hole:
        return C_INNER

    return C_BG


def render_rgba(size):
    """Render size x size RGBA image using 4x4 supersampling."""
    ss = 4 if size <= 256 else 2
    small_optical = size <= 24
    inv = 64.0 / size
    step = inv / ss
    half = step * 0.5
    inv_samples = 1.0 / (ss * ss)

    buf = bytearray(size * size * 4)
    pos = 0
    for py in range(size):
        base_v = py * inv
        for px in range(size):
            base_u = px * inv
            # Fast interior reject/check for large icons: check corners + center
            c0 = sample_point(base_u + half, base_v + half, small_optical)
            if size >= 128:
                c1 = sample_point(base_u + inv - half, base_v + half, small_optical)
                c2 = sample_point(base_u + half, base_v + inv - half, small_optical)
                c3 = sample_point(base_u + inv - half, base_v + inv - half, small_optical)
                if c0 == c1 == c2 == c3:
                    buf[pos:pos + 4] = bytes(c0)
                    pos += 4
                    continue
            ra = ga = ba = aa = 0.0
            for sy in range(ss):
                v = base_v + sy * step + half
                for sx in range(ss):
                    u = base_u + sx * step + half
                    r, g, b, a = sample_point(u, v, small_optical)
                    if a:
                        af = a / 255.0
                        ra += r * af
                        ga += g * af
                        ba += b * af
                        aa += af
            if aa > 0.0:
                a_out = min(255, int(round((aa * inv_samples) * 255.0)))
                r_out = min(255, int(round(ra / aa)))
                g_out = min(255, int(round(ga / aa)))
                b_out = min(255, int(round(ba / aa)))
                buf[pos] = r_out
                buf[pos + 1] = g_out
                buf[pos + 2] = b_out
                buf[pos + 3] = a_out
            pos += 4
    return bytes(buf)


def encode_png(size, rgba):
    def chunk(tag, data):
        crc = zlib.crc32(tag + data) & 0xFFFFFFFF
        return struct.pack('>I4s', len(data), tag) + data + struct.pack('>I', crc)

    stride = size * 4
    raw = bytearray()
    for y in range(size):
        raw.append(0)  # filter type 0 (None)
        start = y * stride
        raw.extend(rgba[start:start + stride])

    ihdr = struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), 9)
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', idat) + chunk(b'IEND', b'')


def encode_ico(png_map, sizes):
    """Pack PNG-compressed entries into a standard Windows .ico file."""
    count = len(sizes)
    header = struct.pack('<HHH', 0, 1, count)
    offset = 6 + 16 * count
    entries = bytearray()
    payloads = bytearray()
    for sz in sizes:
        data = png_map[sz]
        dim = 0 if sz >= 256 else sz
        entries.extend(struct.pack('<BBBBHHII', dim, dim, 0, 0, 1, 32, len(data), offset))
        payloads.extend(data)
        offset += len(data)
    return header + bytes(entries) + bytes(payloads)


def encode_icns(png_map):
    """Pack PNG chunks into a standard macOS .icns container."""
    types = [
        (b'icp4', 16),
        (b'icp5', 32),
        (b'icp6', 64),
        (b'ic07', 128),
        (b'ic08', 256),
        (b'ic09', 512),
        (b'ic10', 1024),
    ]
    body = bytearray()
    for code, sz in types:
        data = png_map[sz]
        body.extend(struct.pack('>4sI', code, 8 + len(data)))
        body.extend(data)
    return struct.pack('>4sI', b'icns', 8 + len(body)) + bytes(body)


def main():
    OUT_DIR.mkdir(parents=True, exist_ok=True)
    all_sizes = [16, 24, 32, 48, 64, 128, 256, 512, 1024]
    png_map = {}
    for sz in all_sizes:
        png_map[sz] = encode_png(sz, render_rgba(sz))

    for sz in [16, 32, 64, 128, 256, 512, 1024]:
        out = OUT_DIR / f'furrow-icon-{sz}.png'
        out.write_bytes(png_map[sz])
        print(f'wrote {out.name} ({len(png_map[sz])} bytes)')

    ico = encode_ico(png_map, [16, 24, 32, 48, 64, 128, 256])
    (OUT_DIR / 'furrow.ico').write_bytes(ico)
    print(f'wrote furrow.ico ({len(ico)} bytes)')

    icns = encode_icns(png_map)
    (OUT_DIR / 'furrow.icns').write_bytes(icns)
    print(f'wrote furrow.icns ({len(icns)} bytes)')


if __name__ == '__main__':
    main()
