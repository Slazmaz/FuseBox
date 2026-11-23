"""Draws FuseBox's icon (a white lightning bolt on a blue rounded tile) into
src/fusebox.ico, for the .exe. The window draws the same icon itself
(set_window_icon in main.c): keep BOLT, BLUE and RADIUS in step with it.

Standard library only: python tools/make_icon.py
"""
import os
import struct
import zlib

# The bolt, in a square from -0.5 to 0.5 (y down): main.c's `bolt`.
BOLT = [(0.14, -0.40), (-0.27, 0.06), (-0.01, 0.06),
        (-0.14, 0.40), (0.27, -0.06), (0.01, -0.06)]
# C_ACCENT.
BLUE = (0.30, 0.56, 1.00)
RADIUS = 0.22
SIZES = [16, 24, 32, 48, 64, 128, 256]
SUB = 4


def in_bolt(x, y):
    inside = False
    j = len(BOLT) - 1
    for i in range(len(BOLT)):
        xi, yi = BOLT[i]
        xj, yj = BOLT[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def render(size):
    rows = []
    for y in range(size):
        row = bytearray([0])  # PNG filter: none
        for x in range(size):
            r = g = b = a = 0.0
            for sy in range(SUB):
                for sx in range(SUB):
                    fx = (x + (sx + 0.5) / SUB) / size - 0.5
                    fy = (y + (sy + 0.5) / SUB) / size - 0.5
                    qx = max(abs(fx) - (0.5 - RADIUS), 0.0)
                    qy = max(abs(fy) - (0.5 - RADIUS), 0.0)
                    if qx * qx + qy * qy > RADIUS * RADIUS:
                        continue
                    a += 1
                    if in_bolt(fx, fy):
                        r += 1; g += 1; b += 1
                    else:
                        r += BLUE[0]; g += BLUE[1]; b += BLUE[2]
            if a:
                row += bytes([int(r / a * 255), int(g / a * 255), int(b / a * 255),
                              int(a * 255 / (SUB * SUB))])
            else:
                row += bytes(4)
        rows.append(bytes(row))
    return b"".join(rows)


def png(size):
    def chunk(kind, data):
        body = kind + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))
    header = struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0)  # 8-bit RGBA
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + chunk(b"IDAT", zlib.compress(render(size), 9)) + chunk(b"IEND", b""))


def main():
    images = [png(s) for s in SIZES]
    # ICO: a header, a directory entry per size, then the PNGs.
    out = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    for size, data in zip(SIZES, images):
        out += struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), offset)
        offset += len(data)
    out += b"".join(images)
    path = os.path.join(os.path.dirname(__file__), "..", "src", "fusebox.ico")
    with open(path, "wb") as f:
        f.write(out)
    print("wrote", os.path.normpath(path))


if __name__ == "__main__":
    main()
