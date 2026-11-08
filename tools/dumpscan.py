# 扫描所有线程栈窗口，找落在 sender.exe 内的返回地址（限定 RVA 集）
import sys, io, struct
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1] if len(sys.argv) > 1 else r'D:\DSH-Workplace\wdumps\sender.exe.16556.dmp')
reader = mf.get_reader()
mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)
me = next(m for m in mods if m.name.lower().endswith('sender.exe'))
base, size = me.baseaddress, me.size

def read_mem(addr, n):
    for fn in (lambda: reader.read(addr, n), lambda: reader.get_memory(addr, n), lambda: reader.get_memory(addr, n).read()):
        try:
            b = fn()
            if b:
                return bytes(b.read()) if hasattr(b, 'read') else bytes(b)
        except Exception:
            continue
    return None

import re
lines = open(r'D:\DSH-Workplace\projector\sender-pc\build\sender.map', encoding='utf-8', errors='replace').read().splitlines()
pat = re.compile(r'^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8,16}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s+f\s+\S+')
syms = sorted((int(m.group(2), 16), m.group(1)) for m in (pat.match(l) for l in lines) if m)

def sym(rva):
    best = None
    for ad, n in syms:
        if ad <= rva:
            best = (ad, n)
        else:
            break
    if best and rva - best[0] < 0x4000:
        n = best[1]
        n = n.split('@@')[0].strip('?') if n.startswith('?') else n
        return n[:60] + f'+0x{rva-best[0]:x}'
    return None

etid = mf.exception.exception_records[0].ThreadId
for t in mf.threads.threads:
    c = t.ContextObject
    rsp = getattr(c, 'Rsp', 0) or 0
    rip = getattr(c, 'Rip', 0) or 0
    buf = read_mem(rsp - 0x10000, 0x12000) or b''   # 深窗口：覆盖 WER 之下的原始帧
    base_scan = rsp - 0x10000
    hits, seen = [], set()
    sp = rsp
    for _ in range(20000):   # 逐 qword 读，缝隙处 read 失败即停
        b = read_mem(sp, 8)
        if not b or len(b) < 8:
            break
        v, = struct.unpack('<Q', b)
        if base <= v < base + size:
            rva = v - base
            s = sym(rva)
            if s and rva not in seen:
                seen.add(rva)
                hits.append(f'stack+{sp-rsp:#x} rva {rva:#x} = {s}')
        sp += 8
    if hits:
        tag = ' <== EXC-THREAD' if t.ThreadId == etid else ''
        print(f'tid={t.ThreadId:#x}{tag}')
        for h in hits[:8]:
            print('   ', h)
