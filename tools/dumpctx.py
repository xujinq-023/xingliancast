# 读崩溃线程寄存器 + rsp 附近栈 qword，并用 sender.map 符号化落在 sender.exe 内的值
import sys, io, struct, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
from minidump.minidumpfile import MinidumpFile

mf = MinidumpFile.parse(sys.argv[1] if len(sys.argv) > 1 else r'D:\DSH-Workplace\wdumps\sender.exe.16556.dmp')
reader = mf.get_reader()
mods = sorted(mf.modules.modules, key=lambda m: m.baseaddress)

def mod_of(a):
    for m in mods:
        if m.baseaddress <= a < m.baseaddress + m.size:
            return f'{m.name}+0x{a - m.baseaddress:x}'
    return None

def read_mem(addr, n):
    for fn in (lambda: reader.read(addr, n), lambda: reader.get_memory(addr, n), lambda: reader.get_memory(addr, n).read()):
        try:
            b = fn()
            if b:
                if hasattr(b, 'read'):
                    b = b.read()
                return bytes(b)
        except Exception:
            continue
    return None

# sender.map 符号表
try:
    lines = open(r'D:\DSH-Workplace\projector\sender-pc\build\sender.map', encoding='utf-8', errors='replace').read().splitlines()
    pat = re.compile(r'^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8,16}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s+f\s+\S+')
    syms = sorted((int(m.group(2), 16), m.group(1)) for m in (pat.match(l) for l in lines) if m)
except Exception:
    syms = []

def sym(a):
    best = None
    for ad, n in syms:
        if ad <= a:
            best = (ad, n)
        else:
            break
    return f'{best[1]}+0x{a-best[0]:x}' if best and a - best[0] < 0x10000 else None

def dem(n):
    return n.split('@@')[0].split('?')[1] if n.startswith('?') else n

tid = mf.exception.exception_records[0].ThreadId
for t in mf.threads.threads:
    if t.ThreadId == tid:
        c = t.ContextObject
        regs = {}
        for r in ('Rax','Rbx','Rcx','Rdx','Rsi','Rdi','Rsp','Rbp','Rip'):
            regs[r] = getattr(c, r, 0) or 0
            print(f'{r:>4} = {regs[r]:#x}', ('  <== ' + (mod_of(regs[r]) or '')) if mod_of(regs[r]) else '')
        rip, rsp = regs['Rip'], regs['Rsp']
        # rip 处反汇编目标：读崩溃指令附近字节
        code = read_mem(rip - 0x20, 0x40)
        if code:
            print(f'\ncode @rip-0x20..rip+0x20:')
            print(' '.join(f'{b:02x}' for b in code))
        print(f'\nstack @rsp:')
        buf = read_mem(rsp, 0x200) or b''
        base_sender = next((m.baseaddress for m in mods if m.name.lower().endswith('sender.exe')), 0)
        for off in range(0, len(buf), 8):
            v = struct.unpack('<Q', buf[off:off+8])[0]
            ann = ''
            m = mod_of(v)
            if m:
                ann = '  <== ' + m
                if v >= base_sender:
                    s = sym(v - base_sender + 0x140000000 - 0x140000000 + (v - base_sender) - (v - base_sender) + (v - base_sender))
                    s = sym(v - base_sender)
                    if s: ann += '  FN=' + dem(s)
            print(f'{rsp+off:#x}: {v:#x}{ann}')
        break
