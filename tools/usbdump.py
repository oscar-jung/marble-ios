#!/usr/bin/env python3
"""Receive a partition dump from the marble shim (cmdline shim.usb=1 shim.dump=NAME).

The shim prints "MARBLE-DUMP-BEGIN <name> <bytes>\\n" on its USB log channel,
then sends exactly <bytes> raw bytes, then "MARBLE-DUMP-END ...". This saves
the raw part to OUT, the surrounding text to OUT.log, and prints the SHA-256.

  tools/usbdump.py OUT [--wait-secs 120]
"""
import argparse
import hashlib
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
import usb.core  # noqa: E402
import usb.util  # noqa: E402
from usblog import EP_IN, find  # noqa: E402

BEGIN = b"MARBLE-DUMP-BEGIN "


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("out")
    ap.add_argument("--wait-secs", type=float, default=120)
    a = ap.parse_args()

    start = time.time()
    dev = None
    while dev is None:
        dev = find()
        if dev is None:
            if time.time() - start > a.wait_secs:
                sys.exit("[usbdump] no device")
            time.sleep(0.2)
    try:
        dev.set_configuration(1)
    except usb.core.USBError:
        pass
    usb.util.claim_interface(dev, 0)
    sys.stderr.write("[usbdump] connected\n")

    text = bytearray()
    log = open(a.out + ".log", "wb")
    # 1. text until the BEGIN line
    while True:
        try:
            chunk = bytes(dev.read(EP_IN, 16384, timeout=60000))
        except usb.core.USBTimeoutError:
            sys.exit("[usbdump] no BEGIN line within 60 s")
        text += chunk
        i = text.find(BEGIN)
        if i >= 0 and text.find(b"\n", i) >= 0:
            break
    j = text.find(b"\n", i)
    name, size = text[i + len(BEGIN):j].decode().split()
    size = int(size)
    log.write(text[:j + 1])
    sys.stdout.write(text[:j + 1].decode("utf-8", "replace"))
    rest = bytes(text[j + 1:])
    if len(rest) > size:
        sys.exit("[usbdump] protocol error: more data than announced in the first chunk")

    # 2. exactly `size` raw bytes
    h = hashlib.sha256()
    got = 0
    t0 = time.time()
    with open(a.out, "wb") as f:
        if rest:
            f.write(rest)
            h.update(rest)
            got = len(rest)
        last = 0
        while got < size:
            want = min(1 << 20, size - got)
            try:
                chunk = bytes(dev.read(EP_IN, want, timeout=15000))
            except usb.core.USBTimeoutError:
                sys.exit(f"[usbdump] stalled at {got} of {size} bytes")
            f.write(chunk)
            h.update(chunk)
            got += len(chunk)
            if got - last >= 64 << 20 or got == size:
                rate = got / (time.time() - t0 + 1e-9) / 1e6
                sys.stderr.write(f"\r[usbdump] {name}: {got >> 20}/{size >> 20} MiB, {rate:.1f} MB/s")
                last = got
    sys.stderr.write("\n")

    # 3. trailing text (END line)
    tail = bytearray()
    try:
        while b"MARBLE-DUMP-END" not in tail or not tail.endswith(b"\n"):
            tail += bytes(dev.read(EP_IN, 16384, timeout=10000))
    except usb.core.USBTimeoutError:
        pass
    log.write(tail)
    log.close()
    sys.stdout.write(tail.decode("utf-8", "replace"))
    print(f"{a.out}: {got} bytes, sha256 {h.hexdigest()}")
    open(a.out + ".sha256", "w").write(f"{h.hexdigest()}  {a.out.rsplit('/', 1)[-1]}\n")
    usb.util.release_interface(dev, 0)


if __name__ == "__main__":
    main()
