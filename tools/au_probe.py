# Reassemble AUs from loopback UDP (PJ v0.3) and report per-AU structure:
# packet count, NAL count, and how many of those NALs are VCL slices (type 1/5).
# >1 slice per AU with separate FIRST|LAST mini-groups explains the horizontal
# split + green bottom on the TV (each slice fed as its own "frame").
# Run: python au_probe.py <port> <secs>   then start sender --ip 127.0.0.1 --port <port>
import socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 45999
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 6.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
s.settimeout(0.5)

FIRST, LAST, IDR = 1, 2, 4
cur_ts = None
buf = bytearray()
pkts = 0
aus = []          # (npkts, [nal_types], idr, size)

def finish():
    global buf, pkts
    if not buf:
        return
    nals = []
    i = 0
    n = len(buf)
    while i + 3 < n:
        if buf[i] == 0 and buf[i+1] == 0 and (buf[i+2] == 1 or (buf[i+2] == 0 and i + 4 < n and buf[i+3] == 1)):
            sc = 3 if buf[i+2] == 1 else 4
            t = buf[i+sc] & 0x1F
            nals.append(t)
            i += sc
        else:
            i += 1
    aus.append((pkts, nals, bool(pkts and buf[0] is not None), len(buf)))
    buf = bytearray()
    pkts = 0

t0 = time.time()
while time.time() - t0 < secs:
    try:
        pkt, _ = s.recvfrom(65535)
    except socket.timeout:
        continue
    if len(pkt) < 16 or pkt[:2] != b'PJ':
        continue
    _, typ, codec, seq, tslo, flags, kind, plen = struct.unpack_from('>HBBIIbbH', pkt, 0)
    if typ != 0:
        continue
    if flags & FIRST:
        if cur_ts is not None and tslo != cur_ts:
            finish()
        elif cur_ts is not None and buf:
            pass  # same-ts multi-NAL AU: keep appending
        cur_ts = tslo
    if cur_ts is None or tslo != cur_ts:
        continue
    buf += pkt[16:16 + plen]
    pkts += 1
    if flags & LAST:
        finish()
        cur_ts = None

finish()
if not aus:
    print('NO AUs captured')
    sys.exit(0)

from collections import Counter
slice_hist = Counter()
for np_, nals, _i, sz in aus:
    vcl = [t for t in nals if t in (1, 5)]
    slice_hist[len(vcl)] += 1
print(f'aus={len(aus)} slices_per_AU_hist={dict(slice_hist)}')
for rec in aus[:6]:
    np_, nals, _i, sz = rec
    print(f'  pkts={np_} size={sz} nals={nals}')
# slice boundary check: parse first_mb_in_slice (ue) of each type-1/5 NAL to
# find slice row starts; horizontal split => slices are row bands.
def ue_golomb(bits, pos):
    z = 0
    while pos < len(bits) and bits[pos] == 0:
        z += 1; pos += 1
    pos += 1
    if z == 0:
        return 0, pos
    v = int(''.join(map(str, bits[pos:pos+z])), 2) if pos + z <= len(bits) else 0
    return (1 << z) - 1 + v, pos

def byte_bits(b):
    return [(b[i >> 3] >> (7 - (i & 7))) & 1 for i in range(len(b) * 8)]

sample = None
for np_, nals, _i, sz in aus:
    if len([t for t in nals if t in (1, 5)]) > 1:
        sample = np_
        break
if sample:
    print('MULTI-SLICE AUs present -> slice-band split confirmed as picture-split suspect')
elif all(len([t for t in nals if t in (1, 5)]) == 1 for _, nals, _, _ in aus):
    print('single slice per AU -> picture-split must come from loss/error propagation')
