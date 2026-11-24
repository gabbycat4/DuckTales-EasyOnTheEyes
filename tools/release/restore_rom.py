"""Rebuild the ROM from the DT_ROM_0..7 secrets (see upload-rom-secret.sh).

    python tools/release/restore_rom.py roms/baserom.nes parts.b64

The workflow writes the secrets into a file. The result must be the one
supported DuckTales (USA) file (README "Supported ROM", the SHA-256 the
build itself checks); anything else stops the build.
"""
import base64
import hashlib
import lzma
import sys

EXPECTED = "97d472f7b2c1088448d1644155b2650a546bad887b3cba571e16d7986824c169"


def main() -> int:
    out, parts = sys.argv[1], sys.argv[2]
    with open(parts, encoding="ascii") as f:
        text = "".join(f.read().split())
    if not text:
        print("No DT_ROM_* secrets: run tools/release/upload-rom-secret.sh first", file=sys.stderr)
        return 1
    rom = lzma.decompress(base64.b64decode(text))
    digest = hashlib.sha256(rom).hexdigest()
    if digest != EXPECTED:
        print(f"The stored ROM is not DuckTales (USA): {digest}", file=sys.stderr)
        return 1
    with open(out, "wb") as f:
        f.write(rom)
    print(f"ROM restored ({len(rom)} bytes, verified)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
