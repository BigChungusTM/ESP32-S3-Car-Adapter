#!/usr/bin/env python3
"""Read-only inventory; does not load code onto a board or call radio functions."""
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[1]
idf = Path(os.environ.get('IDF_PATH', str(Path.home() / '.platformio/packages/framework-espidf')))
out = root / 'evidence' / 'radio-inventory'
out.mkdir(parents=True, exist_ok=True)
prefix = Path.home() / '.platformio/packages/toolchain-xtensa-esp32s3/bin'
nm = shutil.which('xtensa-esp32s3-elf-nm') or str(prefix / 'xtensa-esp32s3-elf-nm')
objdump = shutil.which('xtensa-esp32s3-elf-objdump') or str(prefix / 'xtensa-esp32s3-elf-objdump')
paths = []
for chip in ('esp32', 'esp32s3'):
    paths += sorted((idf / 'components/esp_phy/lib' / chip).glob('*.a'))
paths += sorted((idf / 'components/bt/controller').glob('**/*.a'))
paths += [idf / p for p in (
    'components/bt/CMakeLists.txt',
    'components/bt/controller/esp32c3/bt.c',
    'components/bt/controller/esp32/bt.c',
    'components/bt/include/esp32c3/include/esp_bt.h',
    'components/esp_rom/esp32s3/ld/esp32s3.rom.bt_funcs.ld',
)]
manifest = {'idf_path': str(idf), 'version': (idf / 'version.txt').read_text().strip(), 'files': []}
for path in paths:
    if not path.exists():
        continue
    relative = str(path.relative_to(idf))
    manifest['files'].append({'path': relative, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()})
    name = relative.replace('/', '__')
    if path.suffix == '.a':
        with (out / (name + '.symbols.txt')).open('w') as f:
            subprocess.run([nm, '-g', '--defined-only', str(path)], stdout=f, check=True)
        if 'esp32s3' in relative and path.name == 'libbtbb.a':
            with (out / 's3-btbb-disassembly.txt').open('w') as f:
                subprocess.run([objdump, '-dr', str(path)], stdout=f, check=True)
    else:
        shutil.copyfile(path, out / name)
(out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
exports = {}
for chip in ('esp32', 'esp32s3'):
    exports[chip] = set()
    for file in out.glob(f'*__{chip}__*.symbols.txt'):
        for line in file.read_text().splitlines():
            fields = line.split()
            if len(fields) == 3 and fields[1] in ('T', 'W'):
                exports[chip].add(fields[2])
with (out / 'chip-symbol-comparison.txt').open('w') as f:
    f.write('Combined controller/PHY exports; names do not establish semantics.\n')
    for label, symbols in (
        ('Shared', exports['esp32'] & exports['esp32s3']),
        ('S3 only', exports['esp32s3'] - exports['esp32']),
        ('Original ESP32 only', exports['esp32'] - exports['esp32s3']),
    ):
        f.write(f'\n{label}: {len(symbols)}\n' + '\n'.join(sorted(symbols)) + '\n')
print(f'Inventoried {len(manifest["files"])} files in {out}')
