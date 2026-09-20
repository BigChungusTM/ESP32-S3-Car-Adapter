#!/usr/bin/env python3
"""Capture a bounded serial session, reopening after USB re-enumeration."""
import argparse
from pathlib import Path
import time
import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument('--port', default='/dev/cu.usbmodem101')
parser.add_argument('--seconds', type=float, default=60)
parser.add_argument('--reset', action='store_true')
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
args.output.parent.mkdir(parents=True, exist_ok=True)
end = time.monotonic() + args.seconds
reset = args.reset
with args.output.open('ab', buffering=0) as log:
    while time.monotonic() < end:
        port = serial.Serial()
        port.port = args.port
        port.baudrate = 115200
        port.timeout = .2
        port.dtr = False
        port.rts = False
        try:
            port.open()
            if reset:
                port.reset_input_buffer()
                HardReset(port, uses_usb=True)()
                reset = False
            while time.monotonic() < end:
                data = port.read(port.in_waiting or 1)
                if data:
                    log.write(data)
                    print(data.decode(errors='replace'), end='', flush=True)
        except (serial.SerialException, OSError):
            time.sleep(.5)
        finally:
            port.close()
