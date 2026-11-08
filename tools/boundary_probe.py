# Hunt for false NAL start codes INSIDE video packet payloads (fragmented long
# NALs). If one chunk's tail + next chunk's head (or one chunk alone) contains
# 00 00 01 / 00 00 00 01, the receiver's Annex-B scan would split the slice
# mid-frame — candidate root cause for half-frame decode errors (bottom half
# green). Also tallies: payload-boundary 00-00 pairs across chunks, per-type
# counts. Run: python boundary_probe.py <port> <secs>  + sender --ip 127.0.0.1
import socket, struct, sys, time

port = int(sys.argv[1]) if len(sys.argv) > 1 else 45999
secs = float(sys.argv[2]) if len(sys.argv) > 2 else 8.0

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.bind(('127.0.0.1', port))
s.settimeout(0.5)

FIRST, LAST = 1, 2
tail00 = ''            # previous video chunk payload tail (hex-ish context)
in_au = False
false_sc = 0           # start codes found inside payload
au_count = 0
au_false = 0           # AUs that contain an internal start code (beyond STP)
vid_pkts = 0
cross00 = 0            # prev tail 00 00 + this head 00 -> 00 00 00 (4-byte STP inside)

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
    payload = pkt[16:16 + plen]
    vid_pkts += 1
    if flags & FIRST:
        in_au = True
        head = payload[:4].hex()
        # FIRST packet must begin with the 3-byte start code (00 00 00 01 ok too)
    # scan payload (skip leading STP of FIRST packet)
    body = payload[3:] if (flags & FIRST and len(payload) > 3) else payload
    for i in range(len(body) - 2):
        if body[i] == 0 and body[i+1] == 0 and body[i+2] == 1:
            false_sc += 1
            if au_count < 3 or false_sc <= 5:
                lo = max(0, i - 6); hi = min(len(body), i + 9)
                print(f'FALSE SC in seq={seq} tslo={tslo} flags={flags} at {i}/{len(payload)} ctx={body[lo:hi].hex()}')
    if in_au and (flags & LAST):
        au_count += 1
        in_au = False

print(f'vid_pkts={vid_pkts} aus={au_count} false_startcodes={false_sc}')
