#!/usr/bin/env python3
"""Build the Apple Device Tree for the SWEET kernel running on marble (POCO F5).

ios-sweet's mkadt.py (third_party/ios-sweet/tools, MIT) builds the minimal ADT
the SWEET kernel reads; every MMIO address comes from module constants. This
sets those constants to marble's values (docs/hardware-inventory.md, measured
on the phone in M0-M5a), builds the tree, then adjusts it:

  - /arm-io/gic: redistributor-stride 0x40000 (Gunyah's virtual GICv3, M3;
    read by xnu/marble-patches/1002)
  - / compatible: adds xiaomi,marble and qcom,sm7475 ('xiaomi,sweet' stays:
    SweetPlatformExpert matches it)
  - /cpus: kryo compatible kept for SweetCPU matching; reg = MPIDR Aff1
    (0x000-0x700, as on marble)
  - /sweet-ufs removed: marble's UFS is not brought up yet (root on a ramdisk
    or none)

  mkadt_marble.py OUT.adt [--qemu] [--dump]

--qemu: QEMU virt instead of the phone (GIC 0x08000000/0x080a0000 with the
GICv3 default stride, CNTFRQ 62.5 MHz, one CPU, RAM at 0x100000000), for a
marble-flavoured kernel (framebuffer 0xb8000000) under shim/'s qemu-xnu with
QEMU_FB_BASE=0xb8000000 and -m 8G.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "third_party", "ios-sweet", "tools"))
import adt  # noqa: E402
import mkadt  # noqa: E402

# ---- marble (POCO F5, SM7475) ----
PHYS_BASE = 0x840000000      # /memory bank 3, no reserved-memory inside (M1)
MEM_SIZE = 0x140000000       # 5 GiB; RAM probe 63/63 over all of it (M5a)
GICD, GICD_SIZE = 0x17100000, 0x10000
GICR, GICR_SIZE = 0x17180000, 8 * 0x40000
GICR_STRIDE = 0x40000        # 4 frames per redistributor (M3)
FB, FB_SIZE = 0xB8000000, 0x2B00000   # cont_splash_region (M0)
TIMEBASE = 19200000
VTIMER_INTID = 27            # PPI 11 (M3: 30 IRQs / 300 ms at 100 Hz)

# ---- QEMU virt, for the same kernel ----
QEMU_PHYS_BASE = 0x100000000
QEMU_MEM_SIZE = 0x100000000


def u64(*v):
    return struct.pack("<%dQ" % len(v), *v)


def cstr(s):
    return s.encode() + b"\0"


def set_prop(node, name, value):
    for i, (n, flags, _) in enumerate(node.props):
        if n == name:
            node.props[i] = (n, flags, value)
            return
    node.props.append((name, 0, value))


def child(node, name):
    for c in node.children:
        if c.name == name:
            return c
    raise KeyError(name)


def build(qemu=False):
    mkadt.PHYS_BASE, mkadt.MEM_SIZE = (QEMU_PHYS_BASE, QEMU_MEM_SIZE) if qemu else (PHYS_BASE, MEM_SIZE)
    mkadt.GICD, mkadt.GICD_SIZE = GICD, GICD_SIZE
    mkadt.GICR, mkadt.GICR_SIZE = GICR, GICR_SIZE
    mkadt.FB, mkadt.FB_SIZE = FB, FB_SIZE
    mkadt.TIMEBASE = TIMEBASE
    mkadt.VTIMER_INTID = VTIMER_INTID
    root = mkadt.build(qemu=qemu)

    set_prop(root, "compatible", cstr("D22AP") + cstr("iPhone10,3") + cstr("xiaomi,sweet") +
             cstr("xiaomi,marble") + cstr("qcom,sm7475") + cstr("AppleARM"))
    gic = child(child(root, "arm-io"), "gic")
    if not qemu:
        set_prop(gic, "redistributor-stride", u64(GICR_STRIDE))
    root.children = [c for c in root.children if c.name != "sweet-ufs"]
    return root


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1].startswith("-"):
        sys.exit(__doc__)
    root = build(qemu="--qemu" in sys.argv)
    blob = adt.serialize(root)
    open(sys.argv[1], "wb").write(blob)
    if "--dump" in sys.argv:
        adt.dump(adt.parse(blob)[0])
    print(f"{sys.argv[1]}: {len(blob)} bytes", file=sys.stderr)


if __name__ == "__main__":
    main()
