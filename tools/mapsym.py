import sys, io, re
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')
lines = open(r'D:\DSH-Workplace\projector\sender-pc\build\sender.map', encoding='utf-8', errors='replace').read().splitlines()
pat = re.compile(r'^\s*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8,16}\s+(\S+)\s+([0-9A-Fa-f]{8,16})\s+f\s+\S+')
syms = []
for ln in lines:
    m = pat.match(ln)
    if m:
        syms.append((int(m.group(2), 16), m.group(1)))
syms.sort()
print(f'{len(syms)} symbols')
BASE = 0x140000000
for t in ([int(x, 16) for x in sys.argv[1:]] or [0x5e50, 0x77e5, 0xee20, 0x25a4]):
    va = BASE + t
    best = None
    for a, n in syms:
        if a <= va:
            best = (a, n)
        else:
            break
    print(f'RVA {t:#06x}: ' + (f'{best[1]}\n            (sym RVA {best[0] - BASE:#06x}, +{va - best[0]:#x} into sym)' if best else '?'))
