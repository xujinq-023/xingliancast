# fdk-aac 独立复现：精确复刻 sender OpenEncoder+EncodePcm 序列，定位 NULL channelMode 崩溃
import struct, sys
exe = open(r'D:\DSH-Workplace\projector\sender-pc\build\sender.exe', 'rb').read()
e = struct.unpack_from('<I', exe, 0x3c)[0]
opt_sz = struct.unpack_from('<H', exe, e+20)[0]
sec = e+24+opt_sz
text = None
for i in range(struct.unpack_from('<H', exe, e+6)[0]):
    o = sec+i*40
    nm = exe[o:o+8].rstrip(b'\0').decode()
    va, rawsz, ro = struct.unpack_from('<I', exe, o+12)[0], struct.unpack_from('<I', exe, o+16)[0], struct.unpack_from('<I', exe, o+20)[0]
    if nm == '.text': text = (va, rawsz, ro)
va, rawsz, ro = text
def r2o(r): return ro + (r - va)

def scan_calls(target_rva, lo=0xa100, hi=0xa6c0):  # aacEncInit 区域内 e8 call 到 target 附近
    hits = []
    for r in range(lo, hi):
        off = r2o(r)
        if exe[off] != 0xE8: continue
        rel = struct.unpack_from('<i', exe, off+1)[0]
        t = r + 5 + rel
        if abs(t - target_rva) < 8:
            hits.append(r)
    return hits

print('== AdjustEncSettings prologue full (0x9a10..0x9a108):')
for base in range(0x9a10, 0x9a108, 0x18):
    print(f'{base:#x}: ', ' '.join(f'{x:02x}' for x in exe[r2o(base):r2o(base)+0x18]))
