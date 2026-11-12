"""Check an X11 screenshot (xwd format) for the test's clear color.

usage: check_clear.py <screen.xwd> <r> <g> <b> <min_pixels>
Exits 0 when at least min_pixels pixels are within 3 of the color.
"""
import struct
import sys


def main():
    path, color = sys.argv[1], tuple(int(v) for v in sys.argv[2:5])
    min_pixels = int(sys.argv[5])
    data = open(path, "rb").read()
    hdr = struct.unpack(">25I", data[:100])
    header_size, pw, ph, bpl, ncolors = hdr[0], hdr[4], hdr[5], hdr[12], hdr[19]
    bpp = hdr[11] // 8
    off = header_size + ncolors * 12
    count = 0
    for y in range(ph):
        row = data[off + y * bpl: off + y * bpl + pw * bpp]
        for x in range(pw):
            b, g, r = row[x * bpp], row[x * bpp + 1], row[x * bpp + 2]
            if abs(r - color[0]) <= 3 and abs(g - color[1]) <= 3 and abs(b - color[2]) <= 3:
                count += 1
    print(f"matching pixels: {count} (need {min_pixels})")
    return 0 if count >= min_pixels else 1


if __name__ == "__main__":
    sys.exit(main())
