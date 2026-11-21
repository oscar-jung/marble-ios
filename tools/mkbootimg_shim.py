#!/usr/bin/env python3
"""Pack an Android boot image (header v2 or v4) for `fastboot boot`.

Qualcomm ABL picks its load path from the header version of the image it is
given (BootLinux.c):
  v2: kernel, ramdisk and DTB all come from this file; vendor_boot is not read.
  v4: kernel and ramdisk come from this file; ABL reads the DTB, the vendor
      ramdisk and bootconfig from the phone's own vendor_boot partition, so
      no DTB has to be extracted first. The initrd ABL reports then spans
      vendor ramdisk + our ramdisk + bootconfig.

Layout and defaults follow ios-sweet / open-mapple's sweet images:
base 0, kernel +0x8000, ramdisk +0x1000000, tags +0x100, dtb +0x1f00000,
page size 4096. Written from the AOSP header definition; no AOSP code.
"""
import argparse
import gzip
import hashlib
import io
import struct
import sys

BOOT_MAGIC = b"ANDROID!"
HEADER_V2_SIZE = 1660
HEADER_V4_SIZE = 1584
V4_PAGE = 4096


def os_version_field(version: str, patch_level: str) -> int:
    a, b, c = (int(x) for x in (version.split(".") + ["0", "0"])[:3])
    y, m = (int(x) for x in patch_level.split("-")[:2])
    if not (0 <= a < 128 and 0 <= b < 128 and 0 <= c < 128 and 2000 <= y < 2128 and 1 <= m <= 12):
        sys.exit("bad --os-version / --os-patch-level")
    return ((a << 14) | (b << 7) | c) << 11 | ((y - 2000) << 4) | m


def empty_cpio_gz() -> bytes:
    """A newc cpio holding only the trailer (ABL dislikes a zero-size ramdisk on some builds)."""
    name = b"TRAILER!!!\0"
    hdr = "070701" + "%08X" * 13 % (0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, len(name), 0)
    raw = hdr.encode() + name
    raw += b"\0" * (-len(raw) % 4)
    raw += b"\0" * (-len(raw) % 512)
    buf = io.BytesIO()
    with gzip.GzipFile(fileobj=buf, mode="wb", mtime=0) as gz:
        gz.write(raw)
    return buf.getvalue()


def pad(data: bytes, page: int) -> bytes:
    return data + b"\0" * (-len(data) % page)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--kernel", required=True)
    ap.add_argument("--ramdisk", help="default: empty gzip newc cpio")
    ap.add_argument("--header-version", type=int, choices=(2, 4), default=2)
    ap.add_argument("--dtb", help="v2 only: concatenated DTBs (stock vendor_boot dtb section)")
    ap.add_argument("--cmdline", default="")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=0)
    ap.add_argument("--kernel-offset", type=lambda s: int(s, 0), default=0x8000)
    ap.add_argument("--ramdisk-offset", type=lambda s: int(s, 0), default=0x1000000)
    ap.add_argument("--second-offset", type=lambda s: int(s, 0), default=0xF00000)
    ap.add_argument("--tags-offset", type=lambda s: int(s, 0), default=0x100)
    ap.add_argument("--dtb-offset", type=lambda s: int(s, 0), default=0x1F00000)
    ap.add_argument("--pagesize", type=int, default=4096)
    ap.add_argument("--os-version", default="15.0.0")
    ap.add_argument("--os-patch-level", default="2035-06")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    kernel = open(a.kernel, "rb").read()
    ramdisk = open(a.ramdisk, "rb").read() if a.ramdisk else empty_cpio_gz()
    if kernel[0x38:0x3C] != b"ARMd":
        sys.exit("--kernel is not an arm64 Image")
    if a.header_version == 4:
        write_v4(a, kernel, ramdisk)
        return
    if not a.dtb:
        sys.exit("--dtb is required for header v2")
    dtb = open(a.dtb, "rb").read()
    if dtb[:4] != b"\xd0\x0d\xfe\xed":
        sys.exit("--dtb does not start with an FDT header")
    cmd = a.cmdline.encode()
    if len(cmd) >= 512 + 1024:
        sys.exit("cmdline too long")
    cmdline, extra = cmd[:511], cmd[511:]

    sha = hashlib.sha1()
    for blob in (kernel, ramdisk, b"", b"", dtb):  # kernel, ramdisk, second, recovery_dtbo, dtb
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))
    ident = sha.digest() + b"\0" * 12

    hdr = struct.pack(
        "<8s10I16s512s32s1024sIQIIQ",
        BOOT_MAGIC,
        len(kernel), a.base + a.kernel_offset,
        len(ramdisk), a.base + a.ramdisk_offset,
        0, a.base + a.second_offset,
        a.base + a.tags_offset,
        a.pagesize,
        2,
        os_version_field(a.os_version, a.os_patch_level),
        b"",
        cmdline,
        ident,
        extra,
        0, 0,                 # recovery_dtbo size, offset
        HEADER_V2_SIZE,
        len(dtb), a.base + a.dtb_offset,
    )
    assert len(hdr) == HEADER_V2_SIZE, len(hdr)

    with open(a.output, "wb") as f:
        for part in (hdr, kernel, ramdisk, dtb):
            f.write(pad(part, a.pagesize))
    print(f"{a.output}: kernel {len(kernel)} ramdisk {len(ramdisk)} dtb {len(dtb)} bytes, header v2")


def write_v4(a, kernel: bytes, ramdisk: bytes) -> None:
    if a.dtb:
        sys.exit("--dtb has no place in a v4 boot image (the DTB lives in vendor_boot)")
    cmd = a.cmdline.encode()
    if len(cmd) >= 1536:
        sys.exit("cmdline too long")
    hdr = struct.pack(
        "<8s4I4II1536sI",
        BOOT_MAGIC,
        len(kernel),
        len(ramdisk),
        os_version_field(a.os_version, a.os_patch_level),
        HEADER_V4_SIZE,
        0, 0, 0, 0,           # reserved
        4,                    # header_version
        cmd,
        0,                    # signature_size
    )
    assert len(hdr) == HEADER_V4_SIZE, len(hdr)
    with open(a.output, "wb") as f:
        for part in (hdr, kernel, ramdisk):
            f.write(pad(part, V4_PAGE))
    print(f"{a.output}: kernel {len(kernel)} ramdisk {len(ramdisk)} bytes, header v4 (DTB from vendor_boot)")


if __name__ == "__main__":
    main()
