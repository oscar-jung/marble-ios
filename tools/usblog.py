#!/usr/bin/env python3
"""Receive the marble shim's USB log channel and print/save it.

The shim (boot with cmdline shim.usb=1) enumerates as 1209:0001 "marble shim
log" with one vendor interface and one bulk IN endpoint (0x81) carrying
everything its console prints. This waits for the device, streams the text to
stdout and appends it to a log file, and goes back to waiting when the device
goes away (e.g. reset), so it can stay running across boots.

  tools/usblog.py [--out build-local/logs/usb-<time>.log] [--once] [--quiet-secs N]
  tools/usblog.py --reboot        ask a running shim to reset (PSCI); Android boots

Needs libusb (brew install libusb) and pyusb (pip install pyusb).
--once: exit after the first session. --quiet-secs N (with --once): also exit
when the device sent nothing for N seconds.
"""
import argparse
import datetime
import os
import sys
import time

try:
    import usb.core
    import usb.util
except ImportError:
    sys.exit("pyusb missing: pip install pyusb (and brew install libusb)")

VID, PID = 0x1209, 0x0001
PRODUCT = "marble shim log"
EP_IN = 0x81


def find():
    for dev in usb.core.find(find_all=True, idVendor=VID, idProduct=PID) or []:
        try:
            if usb.util.get_string(dev, dev.iProduct) == PRODUCT:
                return dev
        except (usb.core.USBError, ValueError):
            continue
    return None


def session(dev, out, quiet_secs):
    try:
        dev.set_configuration(1)
    except usb.core.USBError:
        pass  # already configured
    usb.util.claim_interface(dev, 0)
    last = time.time()
    total = 0
    try:
        while True:
            try:
                data = dev.read(EP_IN, 16384, timeout=1000)
            except usb.core.USBTimeoutError:
                if quiet_secs and time.time() - last > quiet_secs:
                    return total
                continue
            if not data:
                continue
            text = bytes(data).decode("utf-8", "replace")
            sys.stdout.write(text)
            sys.stdout.flush()
            out.write(text)
            out.flush()
            total += len(data)
            last = time.time()
    except usb.core.USBError as e:  # device gone (reset, unplug, handoff)
        sys.stderr.write(f"\n[usblog] session ended: {e}\n")
        return total
    finally:
        try:
            usb.util.release_interface(dev, 0)
            usb.util.dispose_resources(dev)
        except usb.core.USBError:
            pass


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
    ap.add_argument("--out", default=os.path.join(root, "build-local", "logs", f"usb-{stamp}.log"))
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--quiet-secs", type=float, default=0)
    ap.add_argument("--wait-secs", type=float, default=0, help="give up if no device appears (0 = forever)")
    ap.add_argument("--reboot", action="store_true", help="send the vendor reboot request and exit")
    a = ap.parse_args()
    if a.reboot:
        dev = find()
        if dev is None:
            sys.exit("[usblog] no shim device to reboot")
        try:
            dev.ctrl_transfer(0x40, ord("R"), 0, 0, None, timeout=2000)
        except usb.core.USBError as e:  # the device may vanish before the status stage is seen
            sys.stderr.write(f"[usblog] reboot request: {e}\n")
        sys.stderr.write("[usblog] reboot requested\n")
        return
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    sys.stderr.write(f"[usblog] waiting for {VID:04x}:{PID:04x} '{PRODUCT}', log -> {a.out}\n")
    start = time.time()
    with open(a.out, "a") as out:
        while True:
            dev = find()
            if dev is None:
                if a.wait_secs and time.time() - start > a.wait_secs:
                    sys.stderr.write("[usblog] no device\n")
                    sys.exit(2)
                time.sleep(0.2)
                continue
            sys.stderr.write(f"[usblog] connected ({dev.bus}:{dev.address})\n")
            out.write(f"\n===== session {datetime.datetime.now().isoformat()} =====\n")
            n = session(dev, out, a.quiet_secs)
            sys.stderr.write(f"[usblog] {n} bytes\n")
            if a.once:
                return
            time.sleep(0.5)


if __name__ == "__main__":
    main()
