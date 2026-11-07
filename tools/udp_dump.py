# PJS media UDP listener / verifier (host-side self-check, no TV needed).
# Usage: python udp_dump.py --port 45999 --secs 6 --out dump.aac
# Verifies 16B header framing, per-stream seq continuity and ADTS frame_length
# self-consistency (pjs-spec v0.3 §3), and writes the raw ADTS stream to a file.
import argparse
import socket
import struct
import time

ap = argparse.ArgumentParser()
ap.add_argument('--port', type=int, default=45690)
ap.add_argument('--bind', default='127.0.0.1')
ap.add_argument('--secs', type=float, default=6.0)
ap.add_argument('--out', default='dump.aac')
a = ap.parse_args()

RATES = [96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
         16000, 12000, 11025, 8000, 7350]

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind((a.bind, a.port))
s.settimeout(0.5)

HDR = '>HBB I I BB H'   # magic, type, codec, seq, ts_lo, flags, kind, payload_len
stat = dict(v=0, m=0, au=0, vgap=0, agap=0, adts_bad=0, bad=0)
vseq = aseq = None
fi = ch = -1
total_adts = 0
end = time.time() + a.secs
aac = open(a.out, 'wb')
written = 0
while time.time() < end:
    try:
        d, _ = s.recvfrom(2048)
    except socket.timeout:
        continue
    if len(d) < 16 or d[:2] != b'PJ':
        stat['bad'] += 1
        continue
    _, typ, codec, seq, ts, flags, kind, plen = struct.unpack(HDR, d[:16])
    pay = d[16:16 + plen]
    if len(pay) != plen:
        stat['bad'] += 1
        continue
    if typ == 0:                                   # VIDEO
        stat['v'] += 1
        if vseq is not None and seq != vseq + 1:
            stat['vgap'] += 1
        vseq = seq
    elif typ == 2:                                 # META
        stat['m'] += 1
    elif typ == 1:                                 # AUDIO: one ADTS frame/packet
        stat['au'] += 1
        if aseq is not None and seq != aseq + 1:
            stat['agap'] += 1
        aseq = seq
        if len(pay) >= 7 and pay[0] == 0xFF and (pay[1] & 0xF0) == 0xF0:
            fi = (pay[2] >> 2) & 0xF
            ch = ((pay[2] & 1) << 2) | ((pay[3] >> 6) & 3)
            flen = ((pay[3] & 3) << 11) | (pay[4] << 3) | ((pay[5] >> 5) & 7)
            if flen == plen:                       # ADTS frame_length self-consistent
                aac.write(pay)
                written += len(pay)
                total_adts += 1
            else:
                stat['adts_bad'] += 1
        else:
            stat['adts_bad'] += 1
    else:
        stat['bad'] += 1
aac.close()

rate = RATES[fi] if 0 <= fi < 13 else -1
print('video pkts=%d gaps=%d | audio pkts=%d gaps=%d adts_bad=%d | meta=%d | hdr_bad=%d'
      % (stat['v'], stat['vgap'], stat['au'], stat['agap'], stat['adts_bad'],
         stat['m'], stat['bad']))
print('audio ok-frames=%d  rate=%d  ch=%d  -> %s  (%d bytes, ~%.1fs)'
      % (total_adts, rate, ch, a.out, written, total_adts * 1024.0 / (rate or 44100)))
