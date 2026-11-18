"""Copies Deckrunner's source into the public repository folder from an allowlist, then scans it. Never copied: lab/
(retail-derived data), investigation/ (working notes, logs, downloaded third-party mods), build outputs.

    python tools/release/stage-source.py <repository folder>
"""
import shutil
import sys
from pathlib import Path

import scan

REPO = Path(__file__).resolve().parents[2]
# (source, destination) folders and files; build outputs are skipped by name.
ALLOW = ['stage0', 'stageA/probe/CMakeLists.txt', 'stageA/probe/src', 'stageA/probe/tests', 'stageA/probe/third_party',
         'stageB/deckrunner-runtime', 'tools/analysis', 'tools/board', 'tools/crash', 'tools/release', 'tools/wkprobe',
         'release', 'media', '.github']
SKIP_DIRS = {'target', 'bin', 'obj', '__pycache__', '.vs', 'out'}
SKIP_SUFFIXES = {'.pyc', '.pdb', '.user', '.log'}


def files():
    for a in ALLOW:
        p = REPO / a
        if p.is_file():
            yield p
            continue
        for f in sorted(p.rglob('*')):
            if f.is_file() and not (SKIP_DIRS & set(f.relative_to(REPO).parts)) and f.suffix.lower() not in SKIP_SUFFIXES:
                yield f


def main():
    dest = Path(sys.argv[1])
    copied = []
    for f in files():
        t = dest / f.relative_to(REPO)
        t.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, t)
        copied.append(t)
    # Source may hold scripts (the release build's own .ps1); private paths and game data never.
    problems = [x for x in scan.scan(copied) if 'a script-host file' not in x]
    print(f'{len(copied)} files staged')
    if problems:
        print('\n'.join(problems))
        sys.exit(1)
    print('scan clean')


if __name__ == '__main__':
    main()
