#!/usr/bin/env python3
"""Flash one built probe, reset via native USB Serial/JTAG, and capture output."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import time
import serial
from esptool.reset import HardReset

parser = argparse.ArgumentParser()
parser.add_argument('--port', required=True)
parser.add_argument('--case', type=int, choices=range(5), required=True)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
build = root / 'firmware/controller-probe' / f'build-{args.case}'
args.output.mkdir(parents=True, exist_ok=True)
flasher = json.loads((build / 'flasher_args.json').read_text())
command = [sys.executable, '-m', 'esptool', '--chip', 'esp32s3',
           '--port', args.port, '--baud', '921600', '--after', 'no_reset',
           'write_flash'] + flasher['write_flash_args']
for offset, filename in flasher['flash_files'].items():
    command += [offset, str(build / filename)]
with (args.output / f'case-{args.case}-flash.log').open('w') as log:
    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)

port = serial.Serial()
port.port = args.port
port.baudrate = 115200
port.timeout = 0.2
port.dtr = False
port.rts = False
port.open()
data = bytearray()
try:
    port.reset_input_buffer()
    HardReset(port, uses_usb=True)()
    deadline = time.monotonic() + 25
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        data.extend(chunk)
        if (f'case={args.case} '.encode() in data
                and b'PROBE done=1' in data and data.endswith(b'\n')):
            break
finally:
    port.close()
    (args.output / f'case-{args.case}-serial.log').write_bytes(data)
print(data.decode(errors='replace'))
if b'PROBE done=1' not in data:
    raise SystemExit('No completion marker; inspect serial log before continuing.')
if f'case={args.case} '.encode() not in data:
    raise SystemExit('Case identity missing or incorrect.')
