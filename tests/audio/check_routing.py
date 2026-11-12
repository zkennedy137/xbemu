"""Check a raw S16LE stereo dump of the routing test.

Right channel: only the 880 Hz source, so it must show 880 Hz at about 8000 peak.
Left channel: 880 Hz at full level plus 440 Hz through a submix at half volume. Both start
at the negative peak, so the left peak is about 8000 + 4000 = 12000. Without the submix
route it would be 16000, so the peak checks the route.
usage: check_routing.py <dump.raw> <seconds>
"""
import struct
import sys


def main():
    data = open(sys.argv[1], "rb").read()
    seconds = float(sys.argv[2])
    frames = len(data) // 4
    vals = struct.unpack("<%dh" % (frames * 2), data[:frames * 4])
    left, right = vals[0::2], vals[1::2]
    r_peak = max(abs(v) for v in right)
    l_peak = max(abs(v) for v in left)
    r_hz = sum(1 for a, b in zip(right, right[1:]) if a < 0 <= b) / seconds
    print(f"frames={frames} right_peak={r_peak} right_hz={r_hz:.1f} left_peak={l_peak}")
    ok = frames >= int(48000 * seconds * 0.95) and abs(r_hz - 880) <= 3 and 7000 <= r_peak <= 8100 \
        and 11500 <= l_peak <= 12500
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
