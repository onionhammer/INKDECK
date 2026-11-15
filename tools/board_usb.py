# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5", "pyusb>=1.2", "libusb-package>=1.0.26"]
# ///
"""The board's USB console: opening it without a reset, and bringing it back when it goes dead.

    uv run tools/board_usb.py check PORT    does the firmware answer? re-enumerate if not
    uv run tools/board_usb.py reset [PORT]  re-enumerate the USB device now

A chip reset sometimes leaves the ESP32-S3's USB-Serial-JTAG controller deaf to the bus while
the firmware runs on (about 1 boot in 25 in a test of port-open resets on 2026-11-15): the port
stays, nothing comes over it, esptool says "No serial data received", and the kernel logs
transaction errors on the board's endpoints. A re-enumeration (the USB port reset a replug
would cause, without cutting power) brings the controller back in a fraction of a second,
without resetting the chip. tools/screenshot.py does this by itself; use `reset` after an
esptool or monitor session finds the console dead.

Imported by tools/screenshot.py for open_console(), alive() and reenumerate().
"""

from __future__ import annotations

import os
import sys
import time

import serial

VID, PID = 0x303A, 0x1001  # Espressif USB-Serial-JTAG


def open_console(port_name: str, reset: bool = False, timeout: float = 0.2) -> serial.Serial:
    """Opens the console without resetting the board, or with one reset if `reset`.

    The USB-Serial-JTAG resets the chip while RTS is set and DTR clear. The OS raises both lines
    on open, and pyserial then clears DTR first: RTS without DTR, a reset, even with
    dtr = rts = False set before open(). Clearing RTS first never passes through that state.
    """
    port = serial.Serial()
    port.port, port.baudrate, port.timeout = port_name, 115200, timeout
    port.dtr = port.rts = False
    port._update_dtr_state = lambda: None  # open() then clears only RTS
    port.open()
    del port._update_dtr_state  # the class method again
    port._update_dtr_state()  # and now DTR
    if reset:
        port.rts = True  # RTS without DTR: chip reset, normal boot
        time.sleep(0.1)
        port.rts = False
    return port


def alive(port: serial.Serial, wait: float = 2.0) -> bool:
    """Whether the firmware answers on the console: dev key h prints a heap line."""
    port.reset_input_buffer()
    port.write(b"h")
    port.flush()
    deadline, buf = time.time() + wait, b""
    while time.time() < deadline:
        buf += port.read(4096)
        if b"heap:" in buf:
            return True
    return False


def reenumerate(port_name: str | None = None, wait: float = 10.0) -> bool:
    """Has macOS/Linux re-enumerate the board (a USB port reset, as if replugged, without
    cutting power), then waits for `port_name` to come back. Returns False if it can't."""
    import libusb_package
    import usb.core

    dev = libusb_package.find(idVendor=VID, idProduct=PID)
    if dev is None:
        print("board_usb: no 303a:1001 device on USB", file=sys.stderr)
        return False
    try:
        dev.reset()
    except usb.core.USBError as e:
        print(f"board_usb: USB reset failed: {e}", file=sys.stderr)
        return False
    if port_name is None:
        time.sleep(1)
        return True
    deadline = time.time() + wait
    time.sleep(0.3)  # the old port goes away first
    while time.time() < deadline:
        if os.path.exists(port_name):
            time.sleep(0.5)  # let the serial driver attach
            return True
        time.sleep(0.1)
    print(f"board_usb: {port_name} didn't come back", file=sys.stderr)
    return False


def main() -> None:
    if len(sys.argv) < 2 or sys.argv[1] not in ("check", "reset"):
        sys.exit(__doc__)
    port_name = sys.argv[2] if len(sys.argv) > 2 else None
    if sys.argv[1] == "reset":
        sys.exit(0 if reenumerate(port_name) else 1)
    if port_name is None:
        sys.exit("check needs the port, e.g. /dev/cu.usbmodem101")
    with open_console(port_name) as port:
        if alive(port):
            print("console ok")
            return
    print("console dead: re-enumerating")
    if not reenumerate(port_name):
        sys.exit(1)
    with open_console(port_name) as port:
        if alive(port):
            print("console ok after re-enumerating")
            return
    sys.exit("still no answer: is the firmware running? (download mode doesn't answer h)")


if __name__ == "__main__":
    main()
