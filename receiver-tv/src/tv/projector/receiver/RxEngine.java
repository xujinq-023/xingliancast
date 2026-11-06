package tv.projector.receiver;

import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.SystemClock;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicLong;

/**
 * PJS media layer receiver (protocol spec, section 3): UDP :45690, 16-byte big-endian
 * header, H.264 Annex-B access units rebuilt by ts_us + FIRST/LAST flags, MediaCodec
 * low-latency decode straight onto the Surface.
 *
 * Threading: rx thread only parses + reassembles and posts completed AUs to the
 * "pjs-pipe" HandlerThread which owns the MediaCodec (async callbacks run there too),
 * so all codec state is single-threaded.
 */
public final class RxEngine {
    public static final int PORT = 45690;
    private static final int H = 16;
    private static final int F_FIRST = 1, F_LAST = 2, F_IDR = 4, F_KEY = 8, F_RETR = 16;
    private static final int T_VIDEO = 0, T_AUDIO = 1, T_META = 2;
    private static final int MAX_AU = 1 << 20;          // 1 MiB per access unit cap
    private static final long IDLE_MS = 30_000L;        // 30s of silence -> listening

    private static final int MSG_FRAME = 1, MSG_CFG = 2, MSG_SURFACE = 3, MSG_TICK = 4,
            MSG_ERR = 5;

    public interface UiSink { void onStatus(String text); }

    private final HandlerThread thread;
    private final Handler pipe;
    private final Handler main;
    private final UiSink sink;

    private volatile DatagramSocket socket;
    private Thread rx;
    private volatile boolean running = true;

    // ---- rx-thread-only state ----
    private final Asm vAsm = new Asm();                 // video packet assembler
    private final Asm mAsm = new Asm();                 // meta packet assembler
    private volatile long lastRxMs;
    private volatile String peer = "-";
    private volatile boolean asmReset = true;           // set by pipe, applied by rx
    private int metaTsHi = -1;                          // ts high bits from latest meta
    private long tsBase, lastTs64;                      // ts_lo -> ts64 wrap correction

    // ---- pipeline-thread state ----
    private MediaCodec codec;
    private MediaCodec.Callback cb;
    private Surface surface;
    private volatile boolean configured;
    private volatile byte[] csdSps, csdPps;             // applied SPS/PPS NAL (no start code)
    private volatile boolean waitingIdr = true;         // pipe sets true, rx clears on IDR enqueue
    private final ArrayDeque<Frame> auQ = new ArrayDeque<>();      // pipe-thread only
    private final ArrayDeque<Integer> inIdx = new ArrayDeque<>();  // free input indices
    private int width, height;

    // ---- stats ----
    private final AtomicLong framesFed = new AtomicLong();
    private final AtomicLong lostSeq = new AtomicLong();
    private final AtomicLong droppedAu = new AtomicLong();
    private final AtomicLong metaCount = new AtomicLong();
    private volatile double fps;
    private long prevFrames;                            // pipe-thread only

    private static final class Frame {
        final byte[] au; final long ts; final boolean idr;
        Frame(byte[] au, long ts, boolean idr) { this.au = au; this.ts = ts; this.idr = idr; }
    }

    /** Per-type packet assembler: every packet must bump seq by exactly 1. */
    private static final class Asm {
        long lastSeq = -1;
        boolean tracked;
        ByteArrayOutputStream buf;
        long curTs = -1;
        boolean idr;

        void reset() { lastSeq = -1; tracked = false; buf = null; curTs = -1; idr = false; }
    }

    public RxEngine(UiSink sink) {
        this.sink = sink;
        this.main = new Handler(Looper.getMainLooper());
        this.thread = new HandlerThread("pjs-pipe");
        this.thread.start();
        this.pipe = new Handler(thread.getLooper(), new Handler.Callback() {
            @Override public boolean handleMessage(Message m) { onPipe(m); return true; }
        });
        startRx();
        pipe.sendEmptyMessageDelayed(MSG_TICK, 1000);
        postStatus();
    }

    // ---------------- public API (call from UI thread) ----------------

    public void setSurface(Surface s) {
        pipe.obtainMessage(MSG_SURFACE, s).sendToTarget();
    }

    public void stop() {
        running = false;
        DatagramSocket s = socket;
        if (s != null) { try { s.close(); } catch (Throwable ignored) { } }
        if (rx != null) rx.interrupt();
        pipe.post(new Runnable() {
            @Override public void run() { releaseCodec(); thread.quit(); }
        });
    }

    // ---------------- rx thread ----------------

    private void startRx() {
        rx = new Thread(new Runnable() {
            @Override public void run() { receiveLoop(); }
        }, "pjs-rx");
        rx.setDaemon(true);
        rx.start();
    }

    private boolean bind() {
        try {
            DatagramSocket s = new DatagramSocket(null);
            s.setReuseAddress(true);
            s.bind(new InetSocketAddress("0.0.0.0", PORT));
            s.setReceiveBufferSize(4 << 20);
            socket = s;
            return true;
        } catch (Throwable t) {
            final Object err = t;
            main.post(new Runnable() {
                @Override public void run() { sink.onStatus("ERROR bind udp:" + PORT + " " + err); }
            });
            return false;
        }
    }

    private void receiveLoop() {
        if (!bind()) return;
        byte[] data = new byte[2048];
        DatagramPacket p = new DatagramPacket(data, data.length);
        while (running) {
            DatagramPacket got;
            try {
                socket.receive(p);
                got = p;
            } catch (Throwable e) {
                if (!running) break;
                try { socket.close(); } catch (Throwable ignored) { }
                try { Thread.sleep(200); } catch (InterruptedException ie) { break; }
                if (!bind()) break;
                continue;
            }
            if (!running) break;
            lastRxMs = SystemClock.elapsedRealtime();
            peer = got.getAddress() == null ? "?" : got.getAddress().getHostAddress();
            if (asmReset) { asmReset = false; vAsm.reset(); mAsm.reset(); }
            parse(got.getData(), got.getOffset(), got.getLength());
        }
        try { socket.close(); } catch (Throwable ignored) { }
    }

    private void parse(byte[] d, int off, int len) {
        if (len < H) return;
        if (d[off] != 0x50 || d[off + 1] != 0x4A) return;      // magic 'P','J'
        int type = d[off + 2] & 0xFF;
        if (type != T_VIDEO && type != T_META) return;         // no audio in M2
        long seq = u32(d, off + 4);
        long tslo = u32(d, off + 8);
        int flags = d[off + 12] & 0xFF;
        int kind = d[off + 13] & 0xFF;
        int plen = (int) u16(d, off + 14);
        if (plen > len - H) plen = len - H;
        if (plen <= 0) return;
        byte[] payload = Arrays.copyOfRange(d, off + H, off + H + plen);
        if (type == T_VIDEO) stepVideo(seq, tslo, flags, kind, payload);
        else stepMeta(seq, tslo, flags, payload);
    }

    /** Video packet -> access-unit reassembly keyed on ts_us. rx thread only. */
    private void stepVideo(long seq, long tslo, int flags, int kind, byte[] payload) {
        boolean first = (flags & F_FIRST) != 0, last = (flags & F_LAST) != 0;
        Asm a = vAsm;
        if (a.tracked) {
            int delta = (int) (seq - a.lastSeq);               // correct across 32-bit wrap
            if (delta <= 0 && (flags & F_RETR) != 0) return;   // stale retransmit copy
            if (delta != 1) {
                if (delta > 1) {                               // lost packets in between
                    lostSeq.addAndGet(delta - 1);
                    a.reset();
                    waitingIdr = true;                         // drop half frames, wait IDR
                }
                if (!first) return;                            // mid-frame after a hole
            }
        } else if (!first) {
            return;                                            // join mid-stream: wait FIRST
        }
        if (first) {
            a.buf = new ByteArrayOutputStream(Math.max(payload.length, 16384));
            a.curTs = tslo;
            a.idr = false;
        } else if (a.buf == null || tslo != a.curTs) {
            a.reset(); waitingIdr = true; return;              // interleaved ts: not our AU
        }
        if (a.buf.size() + payload.length > MAX_AU) {          // oversized: drop, wait IDR
            a.reset(); waitingIdr = true; droppedAu.incrementAndGet(); return;
        }
        a.buf.write(payload, 0, payload.length);
        if ((flags & F_IDR) != 0) a.idr = true;
        a.lastSeq = seq;
        a.tracked = true;

        if (last) {
            byte[] au = a.buf.toByteArray();
            boolean idr = a.idr;
            a.buf = null;
            onFrame(au, ts64(tslo), idr || hasNal(au, 5));     // NAL scan as flag fallback
        }
    }

    /** meta frame payload: [ts_hi u16][SPS/PPS Annex-B]; chunks reassembled by FIRST/LAST. */
    private void stepMeta(long seq, long tslo, int flags, byte[] payload) {
        Asm a = mAsm;
        boolean first = (flags & F_FIRST) != 0, last = (flags & F_LAST) != 0;
        if (a.tracked) {
            int delta = (int) (seq - a.lastSeq);
            if (delta <= 0 && (flags & F_RETR) != 0) return;
            if (delta != 1) {
                if (delta > 1) { lostSeq.addAndGet(delta - 1); a.reset(); }
                if (!first) return;
            }
        } else if (!first) {
            return;
        }
        if (first) {
            a.buf = new ByteArrayOutputStream(Math.max(payload.length, 512));
            a.curTs = tslo;
        } else if (a.buf == null || tslo != a.curTs) {
            a.reset(); return;
        }
        if (a.buf.size() + payload.length > MAX_AU) { a.reset(); return; }
        a.buf.write(payload, 0, payload.length);
        a.lastSeq = seq;
        a.tracked = true;
        if (!last) return;

        byte[] full = a.buf.toByteArray();
        a.buf = null;
        if (full.length < 3) return;
        metaTsHi = (int) u16(full, 0);
        metaCount.incrementAndGet();
        ts64(tslo);                                            // keep the ts base in sync
        Cfg c = extractCfg(Arrays.copyOfRange(full, 2, full.length));
        if (c != null && (!Arrays.equals(c.sps, csdSps) || !Arrays.equals(c.pps, csdPps))) {
            pipe.obtainMessage(MSG_CFG, c).sendToTarget();     // new SPS -> reconfigure
        }
    }

    /** ts_lo + latest meta ts_hi -> monotonic 64-bit timestamp (nearest-wrap rule). */
    private long ts64(long tslo) {
        long base = metaTsHi >= 0 ? ((long) metaTsHi << 32) : tsBase;
        long t = base + tslo;
        if (lastTs64 != 0 && t + (1L << 31) < lastTs64) t += (1L << 32);  // lo wrapped past meta
        tsBase = t & 0xFFFFFFFF00000000L;
        lastTs64 = t;
        return t;
    }

    // ---------------- pipeline thread ----------------

    private void onFrame(byte[] au, long ts, boolean idr) {
        if (!configured) {
            Cfg c = extractCfg(au);                            // first frame carries SPS/PPS
            if (c != null && (!Arrays.equals(c.sps, csdSps) || !Arrays.equals(c.pps, csdPps))) {
                pipe.obtainMessage(MSG_CFG, c).sendToTarget();
            }
        }
        if (waitingIdr && idr) waitingIdr = false;             // first usable frame found
        pipe.obtainMessage(MSG_FRAME, new Frame(au, ts, idr)).sendToTarget();
    }

    private void onPipe(Message m) {
        switch (m.what) {
            case MSG_FRAME: {
                Frame f = (Frame) m.obj;
                if (!configured) break;
                if (waitingIdr && !f.idr) { droppedAu.incrementAndGet(); break; }
                if (f.idr) waitingIdr = false;
                if (auQ.size() >= 60) auQ.clear();             // overflow safety valve
                auQ.add(f);
                pump();
                break;
            }
            case MSG_CFG:
                applyCfg((Cfg) m.obj);
                break;
            case MSG_SURFACE: {
                surface = (Surface) m.obj;
                if (surface != null && csdSps != null && !configured) {
                    applyCfg(new Cfg(csdSps, csdPps));         // staged SPS, surface now ready
                }
                break;
            }
            case MSG_ERR:
                releaseCodec();
                break;
            case MSG_TICK:
                tick();
                break;
        }
    }

    /** Feed queued AUs into free input buffers (all on pipe thread). */
    private void pump() {
        MediaCodec c = codec;
        if (c == null) { auQ.clear(); return; }
        while (!inIdx.isEmpty() && !auQ.isEmpty()) {
            int idx = inIdx.poll();
            Frame f = auQ.poll();
            try {
                ByteBuffer b = c.getInputBuffer(idx);
                if (b == null) continue;
                b.clear();
                b.put(f.au);
                c.queueInputBuffer(idx, 0, f.au.length, f.ts, 0);
                framesFed.incrementAndGet();
            } catch (Throwable e) {
                pipe.sendEmptyMessage(MSG_ERR);
                return;
            }
        }
    }

    private void applyCfg(Cfg c) {
        if (c == null || c.sps == null) return;
        if (configured && Arrays.equals(c.sps, csdSps) && Arrays.equals(c.pps, csdPps)) return;
        csdSps = c.sps;
        csdPps = c.pps;
        Sps s = Sps.parse(c.sps);
        int w = (s != null && s.width > 0 && s.height > 0) ? s.width : 1920;
        int h = (s != null && s.width > 0 && s.height > 0) ? s.height : 1080;
        width = w; height = h;                                 // provisional until format cb
        if (surface == null || !surface.isValid()) {           // hold config, wait for surface
            configured = false;
            waitingIdr = true;
            postStatus();
            return;
        }
        try {
            if (codec == null) {
                codec = MediaCodec.createDecoderByType("video/avc");
            } else {
                codec.flush();                                 // SPS changed: flush + reconfigure
            }
            MediaFormat f = MediaFormat.createVideoFormat("video/avc", w, h);
            f.setByteBuffer("csd-0", ByteBuffer.wrap(withSc(c.sps)));
            if (c.pps != null) f.setByteBuffer("csd-1", ByteBuffer.wrap(withSc(c.pps)));
            f.setInteger(MediaFormat.KEY_MAX_INPUT_SIZE, MAX_AU);
            if (Build.VERSION.SDK_INT >= 29) {
                f.setInteger(MediaFormat.KEY_LOW_LATENCY, 1);
                f.setInteger(MediaFormat.KEY_PRIORITY, 0);     // 0 = realtime
            }
            codec.configure(f, surface, null, 0);
            if (cb == null) {
                cb = new MediaCodec.Callback() {
                    @Override public void onInputBufferAvailable(MediaCodec mc, int index) {
                        inIdx.addLast(index);
                        pump();
                    }
                    @Override public void onOutputBufferAvailable(MediaCodec mc, int index,
                                                                  MediaCodec.BufferInfo info) {
                        try {
                            mc.releaseOutputBuffer(index, true); // render now, no PTS pacing (M2)
                        } catch (Throwable e) {
                            pipe.sendEmptyMessage(MSG_ERR);
                        }
                    }
                    @Override public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                        pipe.sendEmptyMessage(MSG_ERR);
                    }
                    @Override public void onOutputFormatChanged(MediaCodec mc, MediaFormat format) {
                        try {
                            int iw = format.getInteger(MediaFormat.KEY_WIDTH);
                            int ih = format.getInteger(MediaFormat.KEY_HEIGHT);
                            if (iw > 0 && ih > 0) { width = iw; height = ih; postStatus(); }
                        } catch (Throwable ignored) { }
                    }
                };
            }
            codec.setCallback(cb, pipe);
            configured = true;
            waitingIdr = true;                                 // feed from next IDR only
            inIdx.clear();
            auQ.clear();
            codec.start();
        } catch (Throwable e) {
            releaseCodec();
        }
        postStatus();
    }

    private void releaseCodec() {
        if (codec != null) {
            try { codec.stop(); } catch (Throwable ignored) { }
            try { codec.release(); } catch (Throwable ignored) { }
        }
        codec = null;
        configured = false;
        waitingIdr = true;
        inIdx.clear();
        auQ.clear();
        width = 0; height = 0;
        framesFed.set(0);
        prevFrames = 0;
        fps = 0;
        asmReset = true;                                       // rx resets assemblers next packet
        postStatus();
    }

    private void tick() {
        long n = framesFed.get();
        fps = n - prevFrames;                                  // 1s cadence
        prevFrames = n;
        boolean streaming = lastRxMs != 0
                && SystemClock.elapsedRealtime() - lastRxMs < IDLE_MS;
        if (!streaming && configured) releaseCodec();          // 30s silence -> listening
        postStatus();
        pipe.sendEmptyMessageDelayed(MSG_TICK, 1000);
    }

    // ---------------- status line ----------------

    private void postStatus() {
        final String text = buildStatus();
        main.post(new Runnable() {
            @Override public void run() { sink.onStatus(text); }
        });
    }

    private String buildStatus() {
        long lost = lostSeq.get(), fed = framesFed.get();
        String res = (width > 0 && height > 0) ? (width + "x" + height) : "-";
        String tail = "lost-seq=" + lost + " drop-au=" + droppedAu.get()
                + " meta=" + metaCount.get() + " res=" + res + (waitingIdr ? " waitIDR" : "");
        if (fed == 0) return "listening  udp:" + PORT + "  pc=" + peer + "\n" + tail;
        return "READY pc=" + peer + " fps=" + String.format(Locale.US, "%.1f", fps)
                + "  fed=" + fed + "\nudp:" + PORT + " " + tail;
    }

    // ---------------- Annex-B helpers ----------------

    static final class Cfg {
        final byte[] sps, pps;                                 // NAL units WITHOUT start code
        Cfg(byte[] sps, byte[] pps) { this.sps = sps; this.pps = pps; }
    }

    /** Scan Annex-B NAL units, collect first SPS(7) + PPS(8). */
    static Cfg extractCfg(byte[] b) {
        byte[] sps = null, pps = null;
        int i = 0;
        while (i < b.length) {
            int sc = startCodeLen(b, i);
            if (sc == 0) { i++; continue; }
            int body = i + sc;
            if (body >= b.length) break;
            int type = b[body] & 0x1F;
            int next = nextStart(b, body);
            int end = Math.min(next, b.length);
            if (type == 7 && sps == null) sps = Arrays.copyOfRange(b, body, end);
            else if (type == 8 && pps == null) pps = Arrays.copyOfRange(b, body, end);
            i = next;
            if (sps != null && pps != null) break;
        }
        return sps == null ? null : new Cfg(sps, pps);
    }

    static boolean hasNal(byte[] b, int type) {
        int i = 0;
        while (i < b.length) {
            int sc = startCodeLen(b, i);
            if (sc == 0) { i++; continue; }
            int body = i + sc;
            if (body >= b.length) break;
            if ((b[body] & 0x1F) == type) return true;
            i = nextStart(b, body);
        }
        return false;
    }

    private static int nextStart(byte[] b, int from) {
        for (int k = from; k < b.length; k++) if (startCodeLen(b, k) != 0) return k;
        return b.length;
    }

    private static int startCodeLen(byte[] b, int i) {
        if (i + 2 < b.length && b[i] == 0 && b[i + 1] == 0) {
            if (b[i + 2] == 1) return 3;
            if (i + 3 < b.length && b[i + 2] == 0 && b[i + 3] == 1) return 4;
        }
        return 0;
    }

    private static byte[] withSc(byte[] nal) {               // 00 00 00 01 + NAL
        byte[] r = new byte[nal.length + 4];
        r[3] = 1;
        System.arraycopy(nal, 0, r, 4, nal.length);
        return r;
    }

    private static int u16(byte[] d, int i) { return (d[i] & 0xFF) << 8 | (d[i + 1] & 0xFF); }

    private static long u32(byte[] d, int i) {
        return ((long) (d[i] & 0xFF) << 24) | ((d[i + 1] & 0xFF) << 16)
                | ((d[i + 2] & 0xFF) << 8) | (d[i + 3] & 0xFF);
    }
}
