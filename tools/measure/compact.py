"""Shrink a --frame-log (CYCFRAME) to what the smoothing measurements need.

python3 -I tools/measure/compact.py logs/amazon.framelog logs/amazon.compact

Output: per frame, u32 frame number, palette (32), OAM (256), CPU RAM (2048).
About 2.3 KB a frame instead of 135 KB.
"""
import struct
import sys


def main():
    src, dst = sys.argv[1], sys.argv[2]
    n = 0
    with open(src, 'rb') as f, open(dst, 'wb') as out:
        if f.read(8) != b'CYCFRAME':
            raise SystemExit('not a frame log')
        (version,) = struct.unpack('<I', f.read(4))
        nlen = 6 if version >= 2 else 5
        while True:
            head = f.read(12 + 4 * nlen)
            if len(head) < 12 + 4 * nlen:
                break
            frame, _, _, *lens = struct.unpack('<III%dI' % nlen, head)
            pal = f.read(32)
            oam = f.read(256)
            ram = f.read(lens[0])
            f.seek(sum(lens[1:]), 1)
            out.write(struct.pack('<I', frame) + pal + oam + ram[:2048].ljust(2048, b'\0'))
            n += 1
    print('%d frames' % n)


if __name__ == '__main__':
    main()
