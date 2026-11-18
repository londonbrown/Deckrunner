"""Bytes of Cyberpunk2077.exe around an RVA (from the file on disk), and memory from a minidump around addresses.
Usage: python peek.py <exe> <dmp> code <rva> | python peek.py <exe> <dmp> mem <addr> [bytes]
"""
import struct
import sys

exe_path, dmp_path, what = sys.argv[1], sys.argv[2], sys.argv[3]
if what == 'code':
    exe = open(exe_path, 'rb').read()
    pe = struct.unpack_from('<I', exe, 0x3C)[0]
    nsec = struct.unpack_from('<H', exe, pe + 6)[0]
    opt = struct.unpack_from('<H', exe, pe + 20)[0]
    rva = int(sys.argv[4], 16)
    for i in range(nsec):
        o = pe + 24 + opt + i * 40
        vsize, va, rawsize, rawptr = struct.unpack_from('<IIII', exe, o + 8)
        if va <= rva < va + vsize:
            off = rawptr + rva - va
            before = exe[off - 24:off]
            after = exe[off:off + 16]
            print('before:', before.hex(' '))
            print('after: ', after.hex(' '))
else:
    data = open(dmp_path, 'rb').read()
    sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', data, 0)
    ranges = []
    for i in range(nstreams):
        st, size, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
        if st == 5:
            cnt = struct.unpack_from('<I', data, rva)[0]
            for k in range(cnt):
                s, sz, off = struct.unpack_from('<QII', data, rva + 4 + k * 16)
                ranges.append((s, sz, off))
        if st == 9:
            cnt, base_rva = struct.unpack_from('<QQ', data, rva)
            off = base_rva
            for k in range(cnt):
                s, sz = struct.unpack_from('<QQ', data, rva + 16 + k * 16)
                ranges.append((s, sz, off))
                off += sz
    addr = int(sys.argv[4], 16)
    n = int(sys.argv[5]) if len(sys.argv) > 5 else 64
    for s, sz, off in ranges:
        if s <= addr < s + sz:
            b = data[off + addr - s:off + min(addr - s + n, sz)]
            for i in range(0, len(b), 8):
                print(f'  +0x{i:02x}: 0x{struct.unpack_from("<Q", b.ljust(i + 8, bytes(1)), i)[0]:016x}')
            break
    else:
        print('  not in the dump')
