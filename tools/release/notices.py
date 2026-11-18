"""Writes THIRD-PARTY-NOTICES.txt for a Deckrunner release: the credits below, then every crate linked into the shipped
Rust binary (the runtime) with its own licence files, from cargo's resolved
dependency graph, and the RED4ext SDK's licence (the plugin). Fails when a shipped crate has neither licence files nor
a licence field.

    python tools/release/notices.py --out <file> [--sk8v <deckrunner-sk8v checkout>]
"""
import argparse
import json
import subprocess
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TARGET = 'x86_64-pc-windows-msvc'
LICENSE_FILES = ('LICENSE', 'LICENCE', 'COPYING', 'NOTICE', 'UNLICENSE')
RED4EXT_SDK = REPO.parent / 'cp-RED4ext.SDK' / 'LICENSE.md'  # the SDK checkout the plugin builds against

HEADER = """Deckrunner third-party notices
==============================

Deckrunner is licensed under the Apache License, Version 2.0 (LICENSE). It contains or is built from the third-party
work below. It ships no Skate 3 or Cyberpunk 2077 code or data: its setup builds what it needs from the player's own
copies. Path dependencies of the Rust runtime (SK8V's crates and the Skate engine crates) are credited in NOTICE; every
other crate linked into a shipped binary follows with its own licence.

RED4ext.SDK (MIT), linked into the plugin (deckrunner.dll)
---------------------------------------------------------
"""


def crates(manifest, roots):
    meta = json.loads(subprocess.run(['cargo', 'metadata', '--format-version', '1', '--filter-platform', TARGET,
                                      '--manifest-path', str(manifest)], capture_output=True, text=True, check=True).stdout)
    packages = {p['id']: p for p in meta['packages']}
    nodes = {n['id']: n for n in meta['resolve']['nodes']}
    seen, stack = set(), [i for i, p in packages.items() if p['name'] in roots]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        for d in nodes[i]['deps']:
            if any(k['kind'] is None for k in d['dep_kinds']):  # normal dependencies only: what is linked
                stack.append(d['pkg'])
    return [packages[i] for i in seen]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--sk8v', type=Path, default=REPO.parent / 'deckrunner-sk8v')
    a = ap.parse_args()
    shipped = crates(REPO / 'stageB/deckrunner-runtime/Cargo.toml', {'deckrunner-runtime'})
    text = [HEADER, RED4EXT_SDK.read_text(encoding='utf-8').strip(), '']
    done, missing = set(), []
    for p in sorted(shipped, key=lambda p: (p['name'], p['version'])):
        if p['source'] is None or (p['name'], p['version']) in done:  # path crates: credited in NOTICE
            continue
        done.add((p['name'], p['version']))
        root = Path(p['manifest_path']).parent
        files = sorted(f for f in root.iterdir() if f.is_file() and f.name.upper().startswith(LICENSE_FILES))
        if not files and not p.get('license'):
            missing.append(p['name'])
            continue
        text += [f"{p['name']} {p['version']} ({p.get('license') or 'see below'})", '-' * 60]
        if p.get('repository'):
            text.append(p['repository'])
        for f in files:
            text += ['', f.read_text(encoding='utf-8', errors='replace').strip()]
        text.append('')
    if missing:
        raise SystemExit('no licence for: ' + ', '.join(missing))
    a.out.write_text('\n'.join(text) + '\n', encoding='utf-8')
    print(f'{a.out}: {len(done)} crates')


if __name__ == '__main__':
    main()
