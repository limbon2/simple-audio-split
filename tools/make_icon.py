"""Draws the app icons (res/app.ico and res/app_off.ico) with only the standard library.

A rounded tile with a white "one in, two out" split glyph; the off variant is grey.
Run from the repository root:  python tools/make_icon.py
"""
import math
import struct
import zlib
from pathlib import Path

SIZES = [16, 20, 24, 32, 40, 48, 64, 256]
SUPERSAMPLE = 4

# Glyph geometry in unit coordinates: capsules (x0, y0, x1, y1, radius) and dots (x, y, radius).
STROKE = 0.06
SEGMENTS = [
    (0.24, 0.50, 0.47, 0.50, STROKE),
    (0.47, 0.50, 0.71, 0.29, STROKE),
    (0.47, 0.50, 0.71, 0.71, STROKE),
]
DOTS = [(0.24, 0.50, 0.095), (0.73, 0.28, 0.105), (0.73, 0.72, 0.105)]
TILE_MARGIN = 0.03
TILE_RADIUS = 0.23


def seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    t = max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def in_tile(px, py):
    lo, hi, r = TILE_MARGIN, 1.0 - TILE_MARGIN, TILE_RADIUS
    cx = min(max(px, lo + r), hi - r)
    cy = min(max(py, lo + r), hi - r)
    return math.hypot(px - cx, py - cy) <= r


def in_glyph(px, py):
    if any(seg_dist(px, py, x0, y0, x1, y1) <= r for x0, y0, x1, y1, r in SEGMENTS):
        return True
    return any(math.hypot(px - x, py - y) <= r for x, y, r in DOTS)


def render(size, top, bottom):
    """Returns rows of (r, g, b, a) tuples, top row first, premultiplied coverage averaged."""
    rows = []
    n = SUPERSAMPLE
    for y in range(size):
        row = []
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(n):
                for sx in range(n):
                    px = (x + (sx + 0.5) / n) / size
                    py = (y + (sy + 0.5) / n) / size
                    if not in_tile(px, py):
                        continue
                    if in_glyph(px, py):
                        color = (255, 255, 255)
                    else:  # vertical gradient on the tile
                        color = tuple(top[i] + (bottom[i] - top[i]) * py for i in range(3))
                    for i in range(3):
                        acc[i] += color[i]
                    acc[3] += 1
            cover = acc[3] / (n * n)
            if acc[3]:
                rgb = [int(round(acc[i] / acc[3])) for i in range(3)]
            else:
                rgb = [0, 0, 0]
            row.append((rgb[0], rgb[1], rgb[2], int(round(cover * 255))))
        rows.append(row)
    return rows


def png_bytes(rows):
    size = len(rows)
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def bmp_bytes(rows):
    size = len(rows)
    header = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    pixels = b"".join(bytes((b, g, r, a)) for row in reversed(rows) for (r, g, b, a) in row)
    mask_row = ((size + 31) // 32) * 4
    return header + pixels + b"\x00" * (mask_row * size)


def write_ico(path, top, bottom):
    images = []
    for size in SIZES:
        rows = render(size, top, bottom)
        images.append((size, png_bytes(rows) if size >= 256 else bmp_bytes(rows)))
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in images:
        dim = 0 if size >= 256 else size
        out += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(data for _, data in images)
    Path(path).write_bytes(out)
    print(f"wrote {path} ({len(out)} bytes)")


if __name__ == "__main__":
    root = Path(__file__).resolve().parent.parent
    write_ico(root / "res" / "app.ico", top=(59, 130, 246), bottom=(29, 78, 216))
    write_ico(root / "res" / "app_off.ico", top=(156, 163, 175), bottom=(107, 114, 128))
