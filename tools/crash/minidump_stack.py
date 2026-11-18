"""Reads a Windows minidump (as REDEngine's crash reporter writes them) without a debugger: the exception, the
crashing thread's registers, and every stack slot that points into a loaded module's code, as module+offset. For a
call through a null pointer (RIP = 0) the slot at RSP is the caller's return address.

Usage: python minidump_stack.py <Cyberpunk2077.dmp> [slots to scan, default 512]
Lab tool: dumps hold game memory and stay local.
"""
import struct
import sys

STREAM_THREAD_LIST, STREAM_MODULE_LIST, STREAM_EXCEPTION, STREAM_MEMORY64 = 3, 4, 6, 9


def main():
    data = open(sys.argv[1], 'rb').read()
    scan = int(sys.argv[2]) if len(sys.argv) > 2 else 512
    sig, ver, nstreams, dir_rva = struct.unpack_from('<IIII', data, 0)
    assert sig == 0x504D444D, 'not a minidump'
    streams = {}
    for i in range(nstreams):
        st, size, rva = struct.unpack_from('<III', data, dir_rva + i * 12)
        streams.setdefault(st, (rva, size))

    def utf16(rva):
        n = struct.unpack_from('<I', data, rva)[0]
        return data[rva + 4:rva + 4 + n].decode('utf-16-le')

    modules = []
    rva, _ = streams[STREAM_MODULE_LIST]
    n = struct.unpack_from('<I', data, rva)[0]
    for i in range(n):
        base, size = struct.unpack_from('<QI', data, rva + 4 + i * 108)
        name_rva = struct.unpack_from('<I', data, rva + 4 + i * 108 + 20)[0]
        modules.append((base, size, utf16(name_rva).split('\\')[-1]))
    modules.sort()

    def where(addr):
        for base, size, name in modules:
            if base <= addr < base + size:
                return f'{name}+0x{addr - base:x}'
        return None

    # Memory: full-memory (Memory64) or per-range lists.
    ranges = []
    if STREAM_MEMORY64 in streams:
        rva, _ = streams[STREAM_MEMORY64]
        cnt, base_rva = struct.unpack_from('<QQ', data, rva)
        off = base_rva
        for i in range(cnt):
            start, size = struct.unpack_from('<QQ', data, rva + 16 + i * 16)
            ranges.append((start, size, off))
            off += size

    STREAM_MEMORY_LIST = 5
    if STREAM_MEMORY_LIST in streams:
        rva, _ = streams[STREAM_MEMORY_LIST]
        cnt = struct.unpack_from('<I', data, rva)[0]
        for i in range(cnt):
            start, size, off = struct.unpack_from('<QII', data, rva + 4 + i * 16)
            ranges.append((start, size, off))
    # Each thread's stack, as the thread list records it.
    if STREAM_THREAD_LIST in streams:
        rva, _ = streams[STREAM_THREAD_LIST]
        cnt = struct.unpack_from('<I', data, rva)[0]
        for i in range(cnt):
            t = rva + 4 + i * 48
            start, size, off = struct.unpack_from('<QII', data, t + 24)
            ranges.append((start, size, off))

    def read(addr, n):
        for start, size, off in ranges:
            if start <= addr and addr + n <= start + size:
                return data[off + addr - start:off + addr - start + n]
        return None

    rva, _ = streams[STREAM_EXCEPTION]
    tid = struct.unpack_from('<I', data, rva)[0]
    code, flags, rec, addr, nparams = struct.unpack_from('<IIQQI', data, rva + 8)
    params = struct.unpack_from('<15Q', data, rva + 8 + 32)
    ctx_size, ctx_rva = struct.unpack_from('<II', data, rva + 8 + 152)
    print(f'thread {tid}: exception 0x{code:08x} at 0x{addr:x}; params {[hex(p) for p in params[:nparams]]}')
    # x64 CONTEXT: Rax at 0x78 ... Rip at 0xF8.
    names = ['rax', 'rcx', 'rdx', 'rbx', 'rsp', 'rbp', 'rsi', 'rdi', 'r8', 'r9', 'r10', 'r11', 'r12', 'r13', 'r14', 'r15', 'rip']
    regs = dict(zip(names, struct.unpack_from('<17Q', data, ctx_rva + 0x78)))
    for k in names:
        w = where(regs[k])
        print(f'  {k} = 0x{regs[k]:016x}' + (f'  ({w})' if w else ''))
    rsp = regs['rsp']
    print(f'stack from rsp, slots pointing into modules (first {scan} slots):')
    for i in range(scan):
        b = read(rsp + i * 8, 8)
        if b is None:
            print(f'  [stack memory not in the dump at slot {i}]')
            break
        v = struct.unpack('<Q', b)[0]
        w = where(v)
        if w:
            print(f'  rsp+0x{i * 8:03x}: 0x{v:016x}  {w}')
    print('modules of interest:')
    for base, size, name in modules:
        if name.lower() in ('cyberpunk2077.exe', 'red4ext.dll', 'deckrunner_probe.dll', 'deckrunner_runtime.dll'):
            print(f'  {name}: 0x{base:x} .. 0x{base + size:x}')


if __name__ == '__main__':
    main()
