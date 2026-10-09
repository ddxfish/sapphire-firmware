#!/usr/bin/env python3
"""What a flasher needs from one ESP-IDF build, into a flat folder.

    python tools/manifest.py <board> <build-dir> <out> [--name "..."]

Reads the build's flasher_args.json and project_description.json and writes
into <out>, every file named after the board (a release's files share one
flat namespace):

    <board>-manifest.json     ESP Web Tools form, plus our "flash" settings
    <board>-<part>.bin        the parts the manifest names
    <board>-README.md         the board's README, when it has one
    index.json                the boards in <out>, this one added or replaced

Sapphire reads that folder as it is, or the same files as a release's
assets (her DEVICE_FIRMWARE_SOURCE setting). build.sh runs this after every build.
"""
import argparse
import hashlib
import json
import shutil
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
    a.out.mkdir(parents=True, exist_ok=True)
    for old in a.out.glob(f'{a.board}-*'):
        old.unlink()                               # a part the build no longer has
    parts = []
    app = Path(plan.get('app', {}).get('file') or '').name
    for offset, rel in sorted(plan['flash_files'].items(), key=lambda kv: int(kv[0], 16)):
        src = a.build / rel
        name = f'{a.board}-{src.name}'
        shutil.copyfile(src, a.out / name)
        part = {'path': name, 'offset': int(offset, 16), 'sha256': hashlib.sha256(src.read_bytes()).hexdigest()}
        if src.name == app:
            part['app'] = True                     # the program itself: what an update over the air sends
        parts.append(part)
    readme = a.build.resolve().parent / 'README.md'
    title = a.name or (first_heading(readme) if readme.is_file() else '') or about.get('project_name') or a.board
    fs = plan.get('flash_settings', {})
    manifest = {
        'name': title,
        'version': about.get('project_version') or '0',
        'new_install_prompt_erase': False,
        'builds': [{'chipFamily': FAMILY.get(chip, chip.upper()),
                    'flash': {'mode': fs.get('flash_mode', 'keep'), 'size': fs.get('flash_size', 'keep'),
                              'freq': fs.get('flash_freq', 'keep')},
                    'parts': parts}],
    }
    (a.out / f'{a.board}-manifest.json').write_text(json.dumps(manifest, indent=1) + '\n')
    entry = {'id': a.board, 'name': title, 'manifest': f'{a.board}-manifest.json'}
    if readme.is_file():
        shutil.copyfile(readme, a.out / f'{a.board}-README.md')
        entry['readme'] = f'{a.board}-README.md'
    index_path = a.out / 'index.json'
    index = json.loads(index_path.read_text()) if index_path.exists() else {'boards': []}
    index['boards'] = [b for b in index.get('boards', []) if b.get('id') != a.board]
    index['boards'].append(entry)
    index['boards'].sort(key=lambda b: b['id'])
    index_path.write_text(json.dumps(index, indent=1) + '\n')
    print(f"{a.board} {manifest['version']} ({manifest['builds'][0]['chipFamily']}): "
          f"{', '.join(p['path'] for p in parts)} -> {a.out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
