"""Builds deckrunner-setup-tools-<n>.zip: the two programs Deckrunner's setup runs, which Nexus Mods does not allow in a
mod file, for a release of Deckrunner's own GitHub repository. Both are built from SK8V 1.1.10 (commit 6a1a6e2) in the
deckrunner-sk8v fork with local paths remapped (cargo-release); skate-xma is LGPL-2.1-or-later, so its source ships
inside, and SK8V's licence and notice too.

    python tools/release/build-tools.py --number 1
"""
import argparse
import hashlib
import os
import zipfile
from pathlib import Path

import scan

REPO = Path(__file__).resolve().parents[2]
BUILT = Path(os.environ.get('LOCALAPPDATA', '')) / 'Deckrunner/cargo-release/release'
SK8V = REPO.parent / 'deckrunner-sk8v'  # the SK8V fork, beside this repository
README = """Deckrunner setup tools {n}

The two programs Deckrunner's setup runs to read your own Skate 3 disc. Nexus Mods does not allow programs in mod
files, so the setup downloads this file instead and checks it against its SHA-256.

- skate-xma.exe: SK8V's EA-XMA audio decoder, a port of FFmpeg 4.4's xmaframes / WMA Pro decoder (libavcodec),
  Copyright the FFmpeg developers. GNU Lesser General Public License 2.1 or later (licenses/COPYING.LGPL); its complete
  source is in source/skate-xma.
- xex_image.exe: SK8V's Xbox 360 executable reader (Apache-2.0, licenses/SK8V-LICENSE).

Both are built unchanged from SK8V 1.1.10 (https://github.com/becaring/SK8V, commit 6a1a6e2).
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--number', required=True)
    a = ap.parse_args()
    out = REPO / 'build/release' / f'deckrunner-setup-tools-{a.number}.zip'
    out.parent.mkdir(parents=True, exist_ok=True)
    files = {'skate-xma.exe': BUILT / 'skate-xma.exe', 'xex_image.exe': BUILT / 'xex_image.exe',
             'licenses/COPYING.LGPL': SK8V / 'rust/skate-xma/COPYING.LGPL', 'licenses/SK8V-LICENSE': SK8V / 'LICENSE',
             'licenses/SK8V-NOTICE': SK8V / 'NOTICE'}
    src = SK8V / 'rust/skate-xma'
    for f in sorted(src.rglob('*')):
        if f.is_file() and 'target' not in f.relative_to(src).parts:
            files[f'source/skate-xma/{f.relative_to(src).as_posix()}'] = f
    # Everything but the two programs must be free of this machine's paths too.
    problems = [p for p in scan.scan(list(files.values())) if 'a program' not in p]
    if problems:
        raise SystemExit('\n'.join(problems))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        z.writestr('README.txt', README.format(n=a.number))
        for name, f in files.items():
            z.write(f, name)
    data = out.read_bytes()
    print(f'{out}\n  {len(data)} bytes\n  sha256 {hashlib.sha256(data).hexdigest()}')


if __name__ == '__main__':
    main()
