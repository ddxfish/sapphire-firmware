#!/usr/bin/env python3
"""One ESP-IDF build into one file a flasher can write at address 0.

    python tools/pack.py <board> <build-dir> <out> [--name "..."]

Reads the build's flasher_args.json and project_description.json and writes
into <out>:

    <board>-<version>.bin     every part at its own address, blanks (0xFF) between
    index.json                the boards in <out>, this one added or replaced

index.json says, for each board, its version, chip, flash settings, the
file with its size and sha256, and where each part sits inside the file. A
person flashes the file at 0 with any ESP tool. Sapphire reads the same
folder, or the same files as a release's assets (her DEVICE_FIRMWARE_SOURCE
setting), and cuts the parts out. build.sh runs this after every build.
"""
import argparse
import hashlib
import json
import sys
from pathlib import Path

FAMILY = {'esp32': 'ESP32', 'esp32s2': 'ESP32-S2', 'esp32s3': 'ESP32-S3', 'esp32c3': 'ESP32-C3',
          'esp32c6': 'ESP32-C6', 'esp32h2': 'ESP32-H2'}


def first_heading(readme):
    for line in readme.read_text(encoding='utf-8').splitlines():
        if line.startswith('# '):
            return line[2:].strip()
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('board')
    ap.add_argument('build', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('--name', default='')
    a = ap.parse_args()
    plan = json.loads((a.build / 'flasher_args.json').read_text())
    about = json.loads((a.build / 'project_description.json').read_text())
    chip = plan.get('extra_esptool_args', {}).get('chip') or about.get('target') or ''
    version = about.get('project_version') or '0'
    # flasher_args names each part twice: by address, and by what it is ("app", "bootloader", "model")
    kinds = {v['file']: k for k, v in plan.items() if isinstance(v, dict) and v.get('file')}
    image, parts = bytearray(), []
    for offset, rel in sorted(plan['flash_files'].items(), key=lambda kv: int(kv[0], 16)):
        offset, data = int(offset, 16), (a.build / rel).read_bytes()
        if offset < len(image):
            sys.exit(f'{rel} at {offset:#x} runs into the part before it')
        image += b'\xff' * (offset - len(image)) + data
        part = {'name': kinds.get(rel) or Path(rel).stem, 'offset': offset, 'size': len(data),
                'sha256': hashlib.sha256(data).hexdigest()}
        if part['name'] == 'app':
            part['app'] = True                     # the program itself: what an update over the air sends
        parts.append(part)
    readme = a.build.resolve().parent / 'README.md'
    title = a.name or (first_heading(readme) if readme.is_file() else '') or about.get('project_name') or a.board
    fs = plan.get('flash_settings', {})
    a.out.mkdir(parents=True, exist_ok=True)
    index_path = a.out / 'index.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {'boards': []}
    for old in index.get('boards', []):
        if old.get('id') == a.board and old.get('file'):
            (a.out / Path(old['file']).name).unlink(missing_ok=True)    # the version before this one
    name = f'{a.board}-{version}.bin'
    (a.out / name).write_bytes(image)
    entry = {'id': a.board, 'name': title, 'version': version, 'chipFamily': FAMILY.get(chip, chip.upper()),
             'flash': {'mode': fs.get('flash_mode', 'keep'), 'size': fs.get('flash_size', 'keep'),
                       'freq': fs.get('flash_freq', 'keep')},
             'file': name, 'size': len(image), 'sha256': hashlib.sha256(image).hexdigest(), 'parts': parts}
    index['boards'] = sorted([b for b in index.get('boards', []) if b.get('id') != a.board] + [entry],
                             key=lambda b: b['id'])
    index_path.write_text(json.dumps(index, indent=1) + '\n')
    print(f"{a.board} {version} ({entry['chipFamily']}): {name}, {len(image)} bytes, "
          f"{', '.join(p['name'] for p in parts)} -> {a.out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
