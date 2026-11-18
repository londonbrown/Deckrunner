"""Names Cyberpunk2077.exe offsets (from minidump_stack.py) with the nearest function start in the game's own
cyberpunk2077_addresses.json, and names those hashes from Codeware's AddressLib and the RED4ext SDK's AddressHashes
when they know them. A return address lands inside its caller, so the nearest start at or below it is the caller.

Usage: python name_addresses.py <game dir> <codeware src> <red4ext sdk include> 0x7e4ecc 0x272ae2 ...
"""
import json
import re
import struct
import sys
from pathlib import Path

game, codeware, sdk = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
exe = (game / 'bin' / 'x64' / 'Cyberpunk2077.exe').read_bytes()
pe = struct.unpack_from('<I', exe, 0x3C)[0]
nsec = struct.unpack_from('<H', exe, pe + 6)[0]
opt = struct.unpack_from('<H', exe, pe + 20)[0]
sections = []
for i in range(nsec):
    o = pe + 24 + opt + i * 40
    name = exe[o:o + 8].rstrip(b'\0').decode()
    vsize, va = struct.unpack_from('<II', exe, o + 8)
    sections.append((name, va, vsize))

names = {}
for f in list((codeware / 'src').rglob('AddressLib.hpp')) + list(sdk.rglob('AddressHashes.hpp')):
    for m in re.finditer(r'(\w+)\s*=\s*(0x[0-9A-Fa-f]+|\d+)\s*;', f.read_text(errors='ignore')):
        names[int(m.group(2), 0)] = m.group(1)

starts = []
for e in json.load(open(game / 'bin' / 'x64' / 'cyberpunk2077_addresses.json', encoding='utf-8'))['Addresses']:
    seg, off = e['offset'].split(':')
    seg, off = int(seg, 16), int(off, 16)
    if seg < 1 or seg > len(sections):
        continue
    starts.append((sections[seg - 1][1] + off, int(e['hash']), e.get('symbol')))
starts.sort()

import bisect
keys = [s[0] for s in starts]
for a in sys.argv[4:]:
    rva = int(a, 16)
    i = bisect.bisect_right(keys, rva) - 1
    if i < 0:
        print(f'0x{rva:x}: before any known function')
        continue
    start, h, sym = starts[i]
    nxt = starts[i + 1][0] if i + 1 < len(starts) else 0
    label = sym or names.get(h) or '?'
    print(f'0x{rva:x}: {label} (hash {h}) + 0x{rva - start:x}; next known start +0x{nxt - start:x}')
