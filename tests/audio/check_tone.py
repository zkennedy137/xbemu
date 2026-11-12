"""Check a raw S16LE stereo dump of the 440 Hz test tone.

usage: check_tone.py <dump.raw> <expected_hz> <seconds>
Counts rising zero crossings on the left channel and checks the peak level.
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    expect_hz, seconds = float(sys.argv[2]), float(sys.argv[3])
    frames = len(data) // 4
    left = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])[0::2]
    peak = max(abs(v) for v in left) if left else 0
    crossings = sum(1 for a, b in zip(left, left[1:]) if a < 0 <= b)
    hz = crossings / seconds
    print(f"frames={frames} peak={peak} rising_crossings={crossings} ~{hz:.1f} Hz")
    ok = frames >= int(48000 * seconds * 0.95) and abs(hz - expect_hz) <= 2.0 and 7000 <= peak <= 8100
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
