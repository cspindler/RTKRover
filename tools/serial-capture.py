#!/usr/bin/env python
"""Capture a bounded window of ESP32 serial output and exit.

`pio device monitor` cannot be used non-interactively: pyserial's miniterm calls
termios.tcgetattr() on stdin and dies with "Operation not supported on socket"
whenever stdin is not a TTY. This reader needs no terminal, so it works from a
script, a CI job, or an agent tool call.

Pulses DTR/RTS on open to reset the board, so the boot banner is captured rather
than missed. Prefixes each line with seconds since reset.

Usage: serial-capture.py <port> [seconds] [baud]
"""
import sys
import time

import serial

port = sys.argv[1]
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 30.0
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

with serial.Serial(port, baud, timeout=0.2) as ser:
    # Hold the ESP32 in reset briefly, then release: EN low via DTR/RTS.
    ser.dtr = False
    ser.rts = True
    time.sleep(0.1)
    # Drain while still held in reset. Flushing after release would swallow the
    # ROM banner (rst:0x.., boot mode, panic output) that crash triage needs.
    ser.reset_input_buffer()
    ser.rts = False

    start = time.monotonic()
    buf = bytearray()
    # Debug builds block in setup() on "Press any key to continue..."
    # (src/main.cpp). Nudge them past it so unattended capture reaches the
    # interesting output; the firmware drains and discards these bytes.
    kicks = [0.5, 1.5, 3.0]
    while time.monotonic() - start < duration:
        elapsed = time.monotonic() - start
        while kicks and elapsed >= kicks[0]:
            ser.write(b"\n")
            kicks.pop(0)
        chunk = ser.read(1024)
        if not chunk:
            continue
        buf.extend(chunk)
        while b"\n" in buf:
            line, _, buf = buf.partition(b"\n")
            text = line.decode("utf-8", "replace").rstrip("\r")
            print(f"[{time.monotonic() - start:7.3f}] {text}", flush=True)
    if buf:
        text = buf.decode("utf-8", "replace")
        print(f"[{time.monotonic() - start:7.3f}] {text}", flush=True)
