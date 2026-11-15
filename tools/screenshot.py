# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "pillow>=10", "pyusb>=1.2", "libusb-package>=1.0.26"]
# ///
"""Drives the UI over the USB console and saves what the e-paper shows as a PNG.

    uv run tools/screenshot.py COM4 shot.png
    uv run tools/screenshot.py COM4 shot.png --keys ddp --delay 2

--keys are src/devtools.h commands, sent one at a time with --delay seconds between them so
each redraw finishes first: u/d dial up/down, p/P press/hold the wheel, b/B BOOT/hold BOOT.
u/d are the physical wheel, so a landscape screen sees them swapped, as it does the real dial.
The image is the framebuffer as drawn (portrait screens come out 480x800), 1 = paper.
--log prints the firmware's log lines that arrive meanwhile (render timings, errors).
--after sends more keys once the frame is in, e.g. --keys o.... --delay 0.4 --after b grabs
the Sleep screen in its last second, then cancels the power-off.

The board is reset first, so --keys start from a fresh boot on Home; --no-reset drives whatever
is on screen instead (and keeps state, such as the Reader's place). Either way, if the firmware
doesn't answer, the USB device is re-enumerated (tools/board_usb.py) and the run goes on.
"""

from __future__ import annotations

import argparse
import base64
import re
import sys
import time
import zlib

import serial
from PIL import Image

import board_usb

ROTATIONS = {  # GFXcanvas1 rotation -> transpose from the native 800x480 buffer
    0: None,
    1: Image.Transpose.ROTATE_90,
    2: Image.Transpose.ROTATE_180,
    3: Image.Transpose.ROTATE_270,
}
LINE = re.compile(r"#(\d+) ([A-Za-z0-9+/=]+)$")


def read_frame(port: serial.Serial, timeout: float) -> Image.Image | None:
    deadline = time.time() + timeout
    pending = b""
    header = None
    lines: dict[int, str] = {}
    while time.time() < deadline:
        pending += port.read(8192)
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            line = raw.strip().decode("ascii", "replace")
            if line.startswith("#FB "):
                _, w, h, rot = line.split()
                header = (int(w), int(h), int(rot))
                lines = {}
            elif header and line.startswith("#END "):
                w, h, rot = header
                data = b"".join(base64.b64decode(lines[i]) for i in sorted(lines))
                if len(data) != w * h // 8 or zlib.crc32(data) != int(line.split()[1], 16):
                    print(f"corrupt frame ({len(lines)} lines), retrying", file=sys.stderr)
                    return None
                img = Image.frombytes("1", (w, h), data)
                return img.transpose(ROTATIONS[rot]) if ROTATIONS.get(rot) else img
            elif header and (m := LINE.match(line)):
                lines[int(m.group(1))] = m.group(2)
    return None


def pump(port: serial.Serial, seconds: float, echo: bool) -> None:
    """Reads (and optionally prints) console output for `seconds`."""
    deadline = time.time() + seconds
    pending = b""
    while time.time() < deadline:
        pending += port.read(4096)
        while b"\n" in pending:
            raw, pending = pending.split(b"\n", 1)
            if echo and raw.strip():
                print(raw.decode("utf-8", "replace").rstrip())


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("port")
    ap.add_argument("out")
    ap.add_argument("--keys", default="", help="devtools key commands to send first")
    ap.add_argument("--delay", type=float, default=1.5, help="seconds after each key")
    ap.add_argument("--timeout", type=float, default=15)
    ap.add_argument("--log", action="store_true", help="print firmware log lines")
    ap.add_argument("--after", default="", help="devtools keys to send after the frame")
    ap.add_argument("--no-reset", action="store_true", help="don't reset the board first")
    args = ap.parse_args()

    port = board_usb.open_console(args.port, reset=not args.no_reset)
    if not args.no_reset:
        # Let it boot before sending keys: until the console has been quiet for 2.5 s, at most 15 s.
        start = quiet = time.time()
        while time.time() - start < 15 and time.time() - quiet < 2.5:
            if port.readline():
                quiet = time.time()
    if not board_usb.alive(port):
        # A reset sometimes leaves the USB controller deaf while the firmware runs on: a
        # re-enumeration brings it back without another reset.
        port.close()
        print("console dead: re-enumerating the USB device", file=sys.stderr)
        if not board_usb.reenumerate(args.port):
            sys.exit("re-enumerating failed")
        port = board_usb.open_console(args.port)
        if not board_usb.alive(port):
            sys.exit("the firmware doesn't answer on the console")
    with port:
        port.reset_input_buffer()
        for key in args.keys:
            port.write(key.encode())
            port.flush()
            pump(port, args.delay, args.log)
        for _ in range(3):
            port.write(b"s")
            port.flush()
            if (img := read_frame(port, args.timeout)) is not None:
                port.write(args.after.encode())
                port.flush()
                img.save(args.out)
                print(f"saved {args.out} ({img.width}x{img.height})")
                return
    sys.exit("no frame received")


if __name__ == "__main__":
    main()
