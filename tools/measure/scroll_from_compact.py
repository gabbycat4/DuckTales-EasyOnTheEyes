"""Camera per frame from a compact log (compact.py), in the scroll.log form
BOSRecomp/tools/camsim/camsim.py reads: "frame on dx" or "frame off".

python3 -I tools/measure/scroll_from_compact.py logs/amazon.compact > logs/amazon_scroll.txt [--no-lag]

off: the vertical row scroll ($E9 set). --no-lag drops frames on which
Scrooge's world X and the camera both stand still between two moving frames
of one direction while the NMI counter ($92) ticked: the console's lag frames,
which the native player's No slowdown (default) removes.
"""
import struct
import sys

REC = 4 + 32 + 256 + 2048


def main():
    data = open(sys.argv[1], 'rb').read()
    no_lag = '--no-lag' in sys.argv
    ram = [data[i * REC + 292:(i + 1) * REC] for i in range(len(data) // REC)]
    nums = [struct.unpack('<I', data[i * REC:i * REC + 4])[0] for i in range(len(ram))]
    cam = [r[0xF3] * 256 + r[0xF2] for r in ram]
    def dx(i):
        d = cam[i] - cam[i - 1]
        return d - 512 if d > 256 else d + 512 if d < -256 else d
    for i in range(1, len(ram)):
        if ram[i][0xE9]:
            print('%d off' % nums[i])
            continue
        d = dx(i)
        if no_lag and d == 0 and 0 < i < len(ram) - 1:
            a, b = dx(i), dx(i + 1) if i + 1 < len(ram) else 0
            p = dx(i - 1)
            if p and b and (p > 0) == (b > 0):
                continue
        print('%d on %d' % (nums[i], d))


if __name__ == '__main__':
    main()
