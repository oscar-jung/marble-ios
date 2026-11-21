#!/usr/bin/env python3
"""Raw a8r8g8b8 little-endian framebuffer dump -> half-size RGB PNG (stdlib only)."""
import struct
import sys
import zlib


def main() -> None:
    raw_path, w, h, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    raw = open(raw_path, "rb").read()
    stride = w * 4
    if len(raw) < stride * h:
        sys.exit(f"{raw_path}: {len(raw)} bytes, need {stride * h}")
    ow, oh = w // 2, h // 2
    rows = bytearray()
    for y in range(oh):
        line = raw[(2 * y) * stride:(2 * y + 1) * stride]
        rows.append(0)
        for x in range(ow):
            b, g, r = line[8 * x], line[8 * x + 1], line[8 * x + 2]
            rows += bytes((r, g, b))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", ow, oh, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")
    open(out, "wb").write(png)
    print(f"{out}: {ow}x{oh}")


if __name__ == "__main__":
    main()
