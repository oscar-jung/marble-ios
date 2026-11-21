#!/usr/bin/env python3
"""Pack the tk test kernel (shim/tk.c) as an XNU payload for the shim's page 5.

The header is the ios-sweet payload layout (see shim/payload.h), magic
"MARBLXNU". tk is a flat, position-independent image, so it becomes one
segment at virt_base; the "ADT" is a recognisable blob tk can verify:
"MARBLADT", u32 length, u32 0, then byte i = i & 0xff.

  mkpayload_tk.py TK_IMAGE OUT --phys-base 0x840000000 --mem-size 0x140000000
                  [--cmdline "tk.hold=1"] [--prefix N]

--prefix N writes N junk bytes before the payload, to mimic the vendor
ramdisk ABL puts in front of ours with a header-v4 image (the shim has to
find the payload by its magic at any 4-byte offset).
"""
import argparse
import struct
import sys

PAGE = 0x4000
BOOT_LINE_LENGTH = 608
HDR = "<8sII7Q%ds" % BOOT_LINE_LENGTH
SEG = "<4Q"
VIRT_BASE = 0xFFFFFFF007004000  # an XNU-like kernel VA; any page-aligned value works


def main() -> None:
    num = lambda s: int(s, 0)
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tk_image")
    ap.add_argument("out")
    ap.add_argument("--phys-base", type=num, required=True)
    ap.add_argument("--mem-size", type=num, required=True)
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--adt-size", type=num, default=0x10000)
    ap.add_argument("--prefix", type=num, default=0)
    a = ap.parse_args()

    tk = open(a.tk_image, "rb").read()
    if tk[0x38:0x3C] != b"ARMd":
        sys.exit("tk image has no arm64 Image header")
    if a.phys_base % PAGE:
        sys.exit("--phys-base must be 16 KiB aligned")
    vmsize = (len(tk) + PAGE - 1) // PAGE * PAGE
    kernel_end = VIRT_BASE + vmsize

    adt = bytearray(i & 0xFF for i in range(a.adt_size))
    adt[0:16] = b"MARBLADT" + struct.pack("<II", a.adt_size, 0)

    hdr_size = struct.calcsize(HDR) + struct.calcsize(SEG)
    assert struct.calcsize(HDR) == 0x2A8
    tk_off = (hdr_size + 15) // 16 * 16
    adt_off = (tk_off + len(tk) + 15) // 16 * 16
    cmd = a.cmdline.encode()
    if len(cmd) >= BOOT_LINE_LENGTH:
        sys.exit("cmdline too long")

    out = bytearray(adt_off + len(adt))
    struct.pack_into(HDR, out, 0, b"MARBLXNU", 1, 1, VIRT_BASE, VIRT_BASE, adt_off, len(adt),
                     kernel_end, a.phys_base, a.mem_size, cmd)
    struct.pack_into(SEG, out, struct.calcsize(HDR), VIRT_BASE, vmsize, tk_off, len(tk))
    out[tk_off:tk_off + len(tk)] = tk
    out[adt_off:] = adt

    with open(a.out, "wb") as f:
        f.write(bytes((0xA5 ^ i) & 0xFF for i in range(a.prefix)))
        f.write(out)
    print(f"{a.out}: tk {len(tk)} B at VA {VIRT_BASE:#x} -> PA {a.phys_base:#x}, adt {len(adt)} B, "
          f"mem {a.mem_size:#x}, prefix {a.prefix}")


if __name__ == "__main__":
    main()
