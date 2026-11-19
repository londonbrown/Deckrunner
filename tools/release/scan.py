"""Scans files for things a Deckrunner release must not contain: this machine's paths and user name (in ASCII and
UTF-16, as compilers embed them), and game data formats. Used by build-release.py; also runs alone:

    python tools/release/scan.py <file or folder> ...
"""
import os
import re
import sys
from pathlib import Path

# Game data formats (Skate's and CD Projekt Red's): never in a release.
RETAIL = {'.big', '.xex', '.mxb', '.glb', '.archive', '.animgraph', '.rig', '.mesh', '.xbm', '.mi', '.ent', '.app',
          '.iso', '.pcm', '.wav', '.xma'}
# Programs: Nexus Mods quarantines uploads that contain them (0.1.0 was); setup downloads the tools it runs instead.
EXECUTABLES = {'.exe', '.com', '.scr', '.msi', '.pif'}
# Script-host files: VirusTotal's sandbox runs them, and PowerShell started from the Temp folder it unpacks into matched
# four Sigma rules on 0.1.1. The launchers are .cmd files that start setup.py.
SCRIPT_HOSTS = {'.ps1', '.psm1', '.psd1', '.bat', '.vbs', '.vbe', '.js', '.jse', '.wsf', '.wsh', '.hta'}


def needles():
    home = Path.home()
    # This machine's user name and home folder, plus any other private words in DECKRUNNER_PRIVATE_WORDS (separated by ;).
    words = {home.name, str(home), *filter(None, os.environ.get('DECKRUNNER_PRIVATE_WORDS', '').split(';'))}
    out = set()
    for w in words:
        for v in (w, w.replace('\\', '/'), w.lower()):
            out.add(v.encode('utf-8'))
            out.add(v.encode('utf-16-le'))
    return out


def found(needle, data):
    """The needle as a whole word: the user name inside a longer public name (a GitHub handle) is not this machine."""
    if b'\x00' in needle:  # UTF-16: full paths only, matched as they are
        return needle in data
    return re.search(rb'(?<![A-Za-z0-9])' + re.escape(needle) + rb'(?![A-Za-z0-9])', data) is not None


def scan(paths, allow=()):
    """`allow`: files built by the release from Deckrunner's own sources that have a game data format's suffix (the
    weapon wheel's name archive); they're still checked for this machine's paths."""
    bad, ns = [], needles()
    allowed = {Path(a).resolve() for a in allow}
    files = []
    for p in map(Path, paths):
        files += [f for f in p.rglob('*') if f.is_file()] if p.is_dir() else [p]
    for f in files:
        if f.suffix.lower() in EXECUTABLES:
            bad.append(f'{f}: a program ({f.suffix}); Nexus quarantines uploads with programs')
            continue
        if f.suffix.lower() in SCRIPT_HOSTS:
            bad.append(f'{f}: a script-host file ({f.suffix}); antivirus sandboxes flag script hosts run from Temp')
            continue
        if f.suffix.lower() in RETAIL and f.resolve() not in allowed:
            bad.append(f'{f}: a game data format ({f.suffix})')
            continue
        data = f.read_bytes()
        hits = sorted({n.decode('utf-16-le' if b'\x00' in n else 'utf-8', 'replace') for n in ns if found(n, data)})
        if hits:
            bad.append(f'{f}: contains {", ".join(hits)}')
    return bad


if __name__ == '__main__':
    problems = scan(sys.argv[1:])
    print('\n'.join(problems) if problems else 'clean')
    sys.exit(1 if problems else 0)
