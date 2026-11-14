package tv.projector.receiver;

import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.media.MediaCodec;
import android.media.MediaFormat;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.Looper;
import android.os.Message;
import android.os.SystemClock;
import android.util.Log;
import android.view.Surface;

import java.io.ByteArrayOutputStream;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Locale;
import java.util.concurrent.atomic.AtomicInteger;
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
    private static final int T_VIDEO = 0, T_AUDIO = 1, T_META = 2, T_CTRL = 3;
    private static final String TAG = "pjs-rx";
    private static final int MAX_AU = 1 << 20;          // 1 MiB per access unit cap
    private static final long IDLE_MS = 3_000L;         // 3s of silence -> listening (bye packet covers the normal case)

    private static final int MSG_FRAME = 1, MSG_CFG = 2, MSG_SURFACE = 3, MSG_TICK = 4,
            MSG_ERR = 5, MSG_AUDIO = 6, MSG_CTRL_BYE = 7, MSG_CTRL_HELLO = 8;

    /** 界面回调：onStatus 走状态文本；onEvent 走一次性事件（lk/ul/sw）。 */
    public interface UiSink { void onStatus(String text); void onEvent(String ev); }

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
    private int tickN = 0;
    private long lastHelloMs = 0;      // hello 连发 3 次，去重避免重复 reconfig
    private volatile String peer = "-";
    private volatile boolean asmReset = true;           // set by pipe, applied by rx
    private int metaTsHi = -1;                          // ts high bits from latest meta
    private long tsBase, lastTs64;                      // ts_lo -> ts64 wrap correction
    private long lastMetaSeq = -1;                      // sender restart detector (mseq goes back to 1)

    // ---- 上屏节拍（vsync 对齐）----
    /** 每帧一格 = 60Hz；设为 0 则回到「解码完立即上屏」。
     *  实测节拍模式会让画面出现卡顿（鼠标移动越快越明显），已回退到 0。 */
    private static final long RENDER_PACE_NS = 0L;
    /** 视频延迟(ms)对齐音频：TV 音频链路实测 ~150ms（AudioTrack 缓冲 + HAL 145ms），
     *  视频链路只有 ~75ms，音频明显偏晚 → 把视频也推迟同样的量。 */
    private static final long AV_SYNC_MS = 0;   // 0 = 不做音画同步（用户优先保证画面流畅）
    private static final long PACE_LEAD_NS = 2_000_000L; // 首帧/重锚提前量
    private long nextRenderNs = 0;                       // pipe 线程/编解码回调线程使用

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
    private final AtomicLong audioFed = new AtomicLong();
    private final AtomicLong audioDrop = new AtomicLong();
    private final AtomicLong fedIdr = new AtomicLong();

    // ---- audio path (own "pjs-aud" thread, M3): ADTS -> MediaCodec AAC -> AudioTrack ----
    private volatile Handler audh;                     // lazy: created on first audio packet
    private final Discovery disc = new Discovery(android.os.Build.MODEL);
    private volatile long aWrittenBytes = 0;           // 写入 AudioTrack 的 PCM 字节数（测算缓冲延迟）
    private HandlerThread athread;
    private final AtomicInteger aPend = new AtomicInteger();
    private long aLastSeq = -1;                        // rx-thread only
    private volatile long lastAudioMs;
    private volatile long lastFrameWall;     // 最近一次真正解出帧的墙上时间
    private volatile boolean pauseShown;     // 已提示"画面已暂停"
    private volatile long lastRxWall;        // 最近一次收到「当前会话」的包（占线判断用）
    private static final long BUSY_WINDOW_MS = 5000;   // 超过 5 秒没包视为空闲，允许他人接管
    private final java.util.HashMap<String, Long> denied = new java.util.HashMap<>();  // 拒绝节流
    private java.util.concurrent.ExecutorService refuser;   // 「忙」回复独立线程（不在接收线程里发包）
    private volatile DatagramSocket refuseSock;             // 专用 socket，避免和反馈通道抢
    private volatile String pcName = "";      // 电脑端计算机名（由 hn 指令上报）
    private volatile String pcPort = "";      // 电脑端 UDP 端口
    // 独立丢包统计（不影响重组逻辑）：只按"相邻收到的视频序号差"累加
    private long lastSeenVSeq = -1;
    private long lostPkts;
    private long lostReported;
    private volatile long freezeUntil;
    private DatagramSocket fbSock;
    private MediaCodec acodec;                         // aud-thread only
    private AudioTrack atrack;                         // aud-thread only
    private int aRate, aCh;                            // aud-thread only
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
        boolean done;                                       // LAST seen: AU waits for ts boundary

        void reset() { lastSeq = -1; tracked = false; buf = null; curTs = -1; idr = false; done = false; }
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
        disc.start();                       // 局域网发现信标（UDP 45691，每秒一次）
    }

    // ---------------- public API (call from UI thread) ----------------

    public void setSurface(Surface s) {
        pipe.obtainMessage(MSG_SURFACE, s).sendToTarget();
    }

    public void stop() {
        running = false;
        disc.stop();
        DatagramSocket s = socket;
        if (s != null) { try { s.close(); } catch (Throwable ignored) { } }
        if (rx != null) rx.interrupt();
        if (refuser != null) { try { refuser.shutdownNow(); } catch (Throwable ignored) { } refuser = null; }
        DatagramSocket rs = refuseSock;
        if (rs != null) { try { rs.close(); } catch (Throwable ignored) { } refuseSock = null; }
        pipe.post(new Runnable() {
            @Override public void run() { releaseCodec(); thread.quit(); }
        });
        Handler h = audh;
        if (h != null) {
            h.post(new Runnable() {
                @Override public void run() { releaseAudio(); }
            });
            athread.quitSafely();
        }
    }

    // ---------------- rx thread ----------------

    private void startRx() {
        if (refuser == null) {
            refuser = java.util.concurrent.Executors.newSingleThreadExecutor();
        }
        rx = new Thread(new Runnable() {
            @Override public void run() { receiveLoop(); }
        }, "pjs-rx");
        rx.setDaemon(true);
        rx.start();
        Thread fb = new Thread(new Runnable() {
            @Override public void run() { feedbackLoop(); }
        }, "pjs-fb");
        fb.setDaemon(true);
        fb.start();
    }

    /** 每秒把接收端状态回传给 PC（UDP 45692），PC 界面据此显示 TV 侧指标。 */
    private void feedbackLoop() {
        while (running) {
            try {
                String ip = peer;
                if (ip != null && !ip.equals("?") && !ip.equals("-")) {
                    InetAddress dst = InetAddress.getByName(ip);
                    DatagramSocket s = fbSock;
                    if (s == null) {
                        s = new DatagramSocket();
                        fbSock = s;
                    }
                    String txt = "PJS/F1"
                        + "\nfps=" + String.format(Locale.US, "%.1f", fps)
                        + "\nlost=" + (lostPkts - lostReported)
                        + "\ndrop=" + droppedAu.get()
                        + "\nidr=" + fedIdr.get()
                        + "\naud=" + audioFed.get()
                        + "\nadrop=" + audioDrop.get()
                        + "\nres=" + (width > 0 ? width + "x" + height : "-")
                        + "\nneedidr=" + (waitingIdr ? 1 : 0);
                    lostReported = lostPkts;
                    byte[] b = txt.getBytes(StandardCharsets.UTF_8);
                    s.send(new DatagramPacket(b, b.length, dst, 45692));
                }
            } catch (Throwable ignored) { }
            try { Thread.sleep(1000); } catch (InterruptedException e) { break; }
        }
    }

    /** 回复 PC 的 ping（"p<ms>" → "q<ms>"），用于测网络 RTT。 */
    private void sendPong(byte[] payload) {
        try {
            String ip = peer;
            if (ip == null || ip.equals("?") || ip.equals("-")) return;
            String p = new String(payload, StandardCharsets.UTF_8);
            if (!p.startsWith("p")) return;
            DatagramSocket s = fbSock;
            if (s == null) { s = new DatagramSocket(); fbSock = s; }
            byte[] b = ("q" + p.substring(1)).getBytes(StandardCharsets.UTF_8);
            s.send(new DatagramPacket(b, b.length, InetAddress.getByName(ip), 45692));
        } catch (Throwable ignored) { }
    }

    /**
     * 已有电脑在投屏时，通知后来者「电视忙」。
     * 关键：**绝不能在接收线程里做网络发送** —— 给一台新机器发包会触发 ARP 解析，
     * 阻塞数百毫秒甚至 1 秒，期间接收线程不读包，正在投屏的那路流就会卡顿/黑一下（实测 bug）。
     * 所以这里只做「记账」，真正的发送交给独立的单线程执行。
     */
    private void refuse(final InetAddress src, final String ip, long nowW) { refuse(src, ip, nowW, ""); }

    private void refuse(final InetAddress src, final String ip, long nowW, final String why) {
        if (src == null) return;
        Long last = denied.get(ip);
        if (last != null && nowW - last < 1500) return;      // 同一台 1.5 秒内只回一次
        denied.put(ip, nowW);
        Logx.i("busy-refuse " + ip + " (" + why + ")");   // 每 1.5 秒最多一条，别在收包循环里逐包写盘
        java.util.concurrent.ExecutorService ex = refuser;      // 可能已被 stop() 置空
        if (ex == null) return;
        try {
        ex.execute(new Runnable() {
            @Override public void run() {
                try {
                    DatagramSocket s = refuseSock;
                    if (s == null) { s = new DatagramSocket(); refuseSock = s; }
                    byte[] b = "PJS/B1\nbusy=1".getBytes(StandardCharsets.UTF_8);
                    s.send(new DatagramPacket(b, b.length, src, 45692));   // 用已解析的 InetAddress，避免再查一次
                } catch (Throwable ignored) { }
            }
        });
        } catch (Throwable ignored) { }                            // 线程池已关闭等异常不能拖垮接收线程
        main.post(new Runnable() {
            @Override public void run() { sink.onEvent("b2"); }
        });
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
            long nowW = SystemClock.elapsedRealtime();
            InetAddress src = got.getAddress();
            String srcIp = src == null ? "?" : src.getHostAddress();
            String cur = peer;
            boolean hasSession = cur != null && !cur.equals("-") && !cur.equals("?");
            long nowMs2 = System.currentTimeMillis();
            // 活跃 = 收包活跃 或 最近 3 秒真的解出过画面。
            // 只看收包会在「刚连上/短暂空档」时误判为空闲，让新机器抢占（实测会打断当前画面）。
            boolean hot = hasSession && ((lastRxWall > 0 && (nowW - lastRxWall) < BUSY_WINDOW_MS)
                                      || (lastFrameWall > 0 && (nowMs2 - lastFrameWall) < 3000));
            if (hot && !srcIp.equals(cur)) {
                // 已有电脑在投屏：后来者一律拒绝，且不切换会话（先连的优先）
                refuse(src, srcIp, nowW, "peer=" + cur + " rxWall=" + (nowW - lastRxWall) + "ms frameWall=" + (nowMs2 - lastFrameWall) + "ms");
                continue;
            }
            lastRxMs = nowW;
            if (!srcIp.equals(cur)) Logx.i("accept new peer " + srcIp + " (prev=" + cur + ")");
            peer = srcIp;
            lastRxWall = nowW;
            if (asmReset) { asmReset = false; vAsm.reset(); mAsm.reset(); aLastSeq = -1; }
            try { parse(got.getData(), got.getOffset(), got.getLength(), srcIp); }
            catch (Throwable t) { Logx.e("parse failed: " + t); }
        }
        try { socket.close(); } catch (Throwable ignored) { }
    }

    private void parse(byte[] d, int off, int len, String srcIp) {
        if (len < H) return;
        if (d[off] != 0x50 || d[off + 1] != 0x4A) return;      // magic 'P','J'
        int type = d[off + 2] & 0xFF;
        if (type != T_VIDEO && type != T_META && type != T_AUDIO && type != T_CTRL) return;
        long seq = u32(d, off + 4);
        long tslo = u32(d, off + 8);
        int flags = d[off + 12] & 0xFF;
        int kind = d[off + 13] & 0xFF;
        int plen = (int) u16(d, off + 14);
        if (plen > len - H) plen = len - H;
        if (plen <= 0) return;
        byte[] payload = Arrays.copyOfRange(d, off + H, off + H + plen);
        if (type == T_VIDEO) stepVideo(seq, tslo, flags, kind, payload);
        else if (type == T_META) stepMeta(seq, tslo, flags, payload);
        else if (type == T_CTRL) stepCtrl(payload, srcIp);
        else stepAudio(seq, flags, payload);
    }

    /** Control packet (CTRL): "bye" = sender stopped casting; drop the decoder right away so
     *  the TV leaves the frozen frame immediately instead of waiting for the idle timeout. */
    private void stepCtrl(byte[] payload, String srcIp) {
        String s = new String(payload, java.nio.charset.StandardCharsets.UTF_8);
        if (s.startsWith("p")) { sendPong(payload); return; }
        // 只接受「当前会话」发来的 bye / 锁屏事件；被拒绝的后来者也会发 bye，绝不能因此断掉正在投屏的会话
        boolean fromOwner = srcIp == null || srcIp.equals(peer);
        if (!fromOwner && (s.startsWith("bye") || s.startsWith("lk") || s.startsWith("ul") || s.startsWith("sw"))) {
            Logx.i("ignore " + s.substring(0, Math.min(3, s.length())) + " from non-owner " + srcIp + " (owner=" + peer + ")");
            return;
        }
        if (s.startsWith("hi")) {
            pipe.sendEmptyMessage(MSG_CTRL_HELLO);   // 日志放在 pipe 线程，去重后只打一条
            return;
        }
        if (s.startsWith("hn")) {
            String rest = s.substring(2).trim();
            String nm = rest, pt = "";
            int tab = rest.indexOf('\t');
            if (tab > 0) { nm = rest.substring(0, tab).trim(); pt = rest.substring(tab + 1).trim(); }
            if (!nm.isEmpty() && (!nm.equals(pcName) || !pt.equals(pcPort))) {
                pcName = nm; pcPort = pt; postStatus();
            }
            return;
        }
        if (s.startsWith("lk") || s.startsWith("ul") || s.startsWith("sw")) {
            final String ev = s.substring(0, 2);
            main.post(new Runnable() {
                @Override public void run() { sink.onEvent(ev); }
            });
            return;
        }
        if (!s.startsWith("bye")) return;
        Log.i(TAG, "sender said bye -> release codec");
        Logx.i("sender disconnected (bye)");
        pipe.sendEmptyMessage(MSG_CTRL_BYE);
    }

    /** Video packet -> access-unit reassembly keyed on ts_us. rx thread only.
     *  NOTE on sender behaviour (pjs.h send_au): a multi-NAL AU emits EACH NAL as a
     *  FIRST|LAST mini-group sharing one ts, with IDR/KEY flags on every packet.
     *  A group therefore = every packet sharing the same ts; FIRST with same ts while
     *  a buffer is open simply appends (it does not restart it). This also covers the
     *  plain AU-level FIRST..LAST-last-packet semantics. */
    private void stepVideo(long seq, long tslo, int flags, int kind, byte[] payload) {
        // 独立丢包统计：同一序号的双发包 delta=0 不算丢，超过 1000 的跳跃视为重置忽略
        if (lastSeenVSeq >= 0) {
            long d = seq - lastSeenVSeq;
            if (d > 1 && d < 1000) lostPkts += d - 1;
        }
        lastSeenVSeq = seq;
        boolean first = (flags & F_FIRST) != 0, last = (flags & F_LAST) != 0;
        Asm a = vAsm;
        // LAST terminates each NAL, NOT the AU: the sender emits every NAL of a frame as
        // its own FIRST|LAST mini-group sharing one ts (loopback probe: IDR AU = 2 slice
        // NALs, nals=[5,5]). Feeding a trailing slice standalone makes VOMX log
        // "get Error Frame" on every output and paints green noise below the slice row.
        // The whole AU is delivered at the ts boundary instead.
        if (a.buf != null && a.done && tslo != a.curTs) {
            byte[] au = a.buf.toByteArray();
            boolean idr = a.idr;
            a.buf = null; a.done = false;
            onFrame(au, ts64(a.curTs), idr || hasNal(au, 5));
        }
        if (a.tracked) {
            int delta = (int) (seq - a.lastSeq);               // correct across 32-bit wrap
            if (delta <= 0) return;                           // late dup: RETR copy or reordered
                                                              // original. Out-of-order originals must
                                                              // NOT append: they land out of position
                                                              // inside the open group (corrupt AU →
                                                              // green-noise frames on the TV).
            if (delta != 1) {
                if (delta > 1 && a.buf != null) {              // reordering skew: sender burst + WiFi
                    a.reset();                                 // reorders; drop the partial AU, NOT
                    a.lastSeq = seq;                           // a real loss and NOT waitingIdr —
                    a.tracked = true;                          // counting skew as loss starved the
                    return;                                    // decoder (drop-au 354 / lost-seq 0
                }                                              // → half-screen green noise).
                if (delta > 1) lostSeq.addAndGet(delta - 1);   // real hole (AU already closed)
                if (!first) return;                            // mid-group after a hole
            }
        } else if (!first) {
            return;                                            // join mid-stream: wait FIRST
        }
        if (first && a.buf != null && tslo == a.curTs) {
            // same-ts continuation (multi-NAL AU): keep appending to the open group
        } else if (first) {
            a.buf = new ByteArrayOutputStream(Math.max(payload.length, 16384));
            a.curTs = tslo;
            a.idr = false;
        } else if (a.buf == null || tslo != a.curTs) {
            // Orphan mid/last packet: its group was already completed (RETR copy tail or
            // reordered original of a KEY AU — sender sends every KEY packet twice) or it
            // belongs to a foreign ts. Nothing to append to: advance lastSeq, drop quietly,
            // and do NOT wait for IDR. Only a decoder (re)config needs a fresh IDR;
            // starving P frames on orphans froze delivery at ~5% (drop-au 34884).
            a.buf = null; a.done = false;                      // stale partial AU from foreign ts
            a.lastSeq = seq;
            a.tracked = true;
            return;
        }
        if (a.buf.size() + payload.length > MAX_AU) {          // oversized: drop, wait IDR
            Log.w(TAG, "resync AU>1MB size=" + a.buf.size());
            a.reset(); waitingIdr = true; droppedAu.incrementAndGet(); return;
        }
        a.buf.write(payload, 0, payload.length);
        if ((flags & F_IDR) != 0) a.idr = true;
        a.lastSeq = seq;
        a.tracked = true;

        if (last) {
            a.done = true;                                     // deliver at next ts boundary
        }
    }

    // rx-thread cfg accumulation: pjs.h emits SPS and PPS as separate same-ts groups,
    // so merge them until a complete (sps+pps) pair is available before reconfiguring.
    private byte[] pendingSps, pendingPps;

    private void noteCfg(byte[] au) {
        Cfg c = extractCfg(au);
        if (c == null) return;
        if (c.sps != null) pendingSps = c.sps;
        if (c.pps != null) pendingPps = c.pps;
        if (pendingSps != null && pendingPps != null
                && (!Arrays.equals(pendingSps, csdSps) || !Arrays.equals(pendingPps, csdPps))) {
            pipe.obtainMessage(MSG_CFG, new Cfg(pendingSps, pendingPps)).sendToTarget();
        }
    }

    /** meta frame payload: [ts_hi u16][SPS/PPS Annex-B]; chunks reassembled by FIRST/LAST. */
    private void stepMeta(long seq, long tslo, int flags, byte[] payload) {
        // Sender restart detection: pjs.h restarts mseq at 1, so the video assembler's
        // lastSeq (large) makes every new packet look like a duplicate (delta <= 0) and
        // the picture freezes until the app is restarted. Reset on a backwards mseq and
        // force a decoder reconfigure; the sender's first AU after restart is an IDR.
        if (lastMetaSeq > 0 && seq < lastMetaSeq && lastMetaSeq - seq > 1000) {
            lastMetaSeq = seq;
            asmReset = true;                       // rx loop resets assemblers + aLastSeq
            pipe.sendEmptyMessage(MSG_ERR);        // release codec -> reconfig on next SPS/IDR
            return;                                // drop this stale packet
        }
        lastMetaSeq = seq;
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
        noteCfg(Arrays.copyOfRange(full, 2, full.length));     // new SPS/PPS -> reconfigure
    }

    /** Audio packets carry exactly one ADTS frame (pjs-spec §3, codec=1): verify seq,
     *  then hand straight to the audio thread. AAC survives single-frame loss, so no
     *  waitingIdr-style resync gate is needed. rx thread only. */
    private void stepAudio(long seq, int flags, byte[] payload) {
        if (payload.length < 8 || (payload[0] & 0xFF) != 0xFF || (payload[1] & 0xF0) != 0xF0) return;
        if (aLastSeq >= 0) {
            int delta = (int) (seq - aLastSeq);
            if (delta <= 0 && (flags & F_RETR) != 0) return;   // stale retransmit copy
            if (delta > 1) lostSeq.addAndGet(delta - 1);       // gap = lost AAC frames (play-through)
        }
        aLastSeq = seq;
        lastAudioMs = SystemClock.elapsedRealtime();
        Handler h = audh;
        if (h == null) {
            synchronized (this) {
                if (audh == null) {
                    athread = new HandlerThread("pjs-aud");
                    athread.start();
                    audh = new Handler(athread.getLooper(), new Handler.Callback() {
                        @Override public boolean handleMessage(Message m) { onAud(m); return true; }
                    });
                }
                h = audh;
            }
        }
        if (aPend.incrementAndGet() > 10) {                    // >210ms backlog: flush for latency
            h.removeMessages(MSG_AUDIO);
            audioDrop.addAndGet(aPend.getAndSet(1));
        }
        h.obtainMessage(MSG_AUDIO, payload).sendToTarget();
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
        noteCfg(au);                                       // in-band SPS/PPS (first frame too)
        // Defensive (old senders): config-only groups (SPS/PPS, no VCL NAL) never get
        // fed — low-latency HW decoders echo garbage frames for them. SPS/PPS arrive
        // via META; feeding slices is all the decoder needs.
        if (!hasNal(au, 1) && !hasNal(au, 5)) return;
        if (waitingIdr && idr) waitingIdr = false;         // first usable frame found
        Message fm = pipe.obtainMessage(MSG_FRAME, new Frame(au, ts, idr));
        if (AV_SYNC_MS > 0) pipe.sendMessageDelayed(fm, AV_SYNC_MS);   // 音画同步：视频整体推迟
        else fm.sendToTarget();
    }

    private void onPipe(Message m) {
        switch (m.what) {
            case MSG_FRAME: {
                Frame f = (Frame) m.obj;
                if (!configured) break;
                // Merge same-ts groups before feeding. The sender emits EACH NAL of a
                // multi-NAL AU as its own FIRST|LAST mini-group sharing one ts; the
                // software MFT encodes IDR pictures as 2 slice NALs (loopback probe:
                // nals=[5,5]). Feeding slice 2 standalone breaks the reference chain
                // below its boundary — TV showed a clean top band + green-noise bottom.
                Frame prev = auQ.peekLast();
                if (prev != null && prev.ts == f.ts
                        && prev.au.length + f.au.length <= MAX_AU) {
                    byte[] j = new byte[prev.au.length + f.au.length];
                    System.arraycopy(prev.au, 0, j, 0, prev.au.length);
                    System.arraycopy(f.au, 0, j, prev.au.length, f.au.length);
                    auQ.pollLast();
                    f = new Frame(j, f.ts, prev.idr || f.idr);
                }
                if (waitingIdr && !f.idr) { droppedAu.incrementAndGet(); break; }
                if (f.idr) {
                    if (waitingIdr) Log.i(TAG, "IDR recovered, drop=" + droppedAu.get());
                    waitingIdr = false;
                    fedIdr.incrementAndGet();
                }
                if (auQ.size() >= 60) {
                    Log.w(TAG, "auQ overflow clear size=" + auQ.size() + " fed=" + framesFed.get());
                    auQ.clear();                             // overflow safety valve
                }
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
                Log.w(TAG, "MSG_ERR: releasing codec for reconfig");
                Logx.w("releasing codec for reconfig");
                releaseCodec();
                break;
            case MSG_CTRL_HELLO: {
            emitEvent("hi");
                // 新会话：复位重组器（旧会话的 seq 全部作废）+ 重建解码器，等下一个 IDR
                long nowMs = SystemClock.elapsedRealtime();
                if (nowMs - lastHelloMs < 1500) break;   // 发送端连发 3 个 hello，只处理第一个
                lastHelloMs = nowMs;
                // 关键：已有画面在正常解码时，绝不因为「别的电脑发来 hello」而重建解码器。
                // 重建会让当前流丢到下一个 IDR（实测约 1 秒黑屏/冻结），表现为"第二台电脑连入时电视断开一秒"。
                if (lastFrameWall > 0 && (System.currentTimeMillis() - lastFrameWall) < 2000 && !waitingIdr) {
                    Log.i(TAG, "hello ignored: session is active");
                    Logx.i("hello ignored (session active, frameWall ago=" + (System.currentTimeMillis() - lastFrameWall) + "ms)");
                    break;
                }
                Logx.i("hello -> reset codec + wait IDR");
                Log.i(TAG, "sender hello -> reset + reconfig");
                Logx.i("sender connected (hello)");
                asmReset = true;
                lastMetaSeq = -1;
                releaseCodec();
                waitingIdr = true;
                postStatus();
                break;
            }
            case MSG_CTRL_BYE: {
                emitEvent("bye");
                // 投屏端点了「停止投屏」：先保留最后一张画面（电视端显示断线提示条），
                // 3.5 秒后再释放解码器，避免立刻黑屏。
                peer = "-";
                lastRxMs = 0;
                framesFed.set(0);                                  // 状态立刻变 "listening"
                freezeUntil = SystemClock.elapsedRealtime() + 3500; // 这段时间内不要掉解码器
                postStatus();
                break;
            }
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
            lastFrameWall = System.currentTimeMillis();
            } catch (Throwable e) {
                Log.w(TAG, "feed failed len=" + f.au.length, e);
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
                            // 上屏节拍调度：立即上屏时，输入(≈60fps)与面板 vsync 相位漂移，
                            // 每几百毫秒会出现一次「重复帧/跳帧」，鼠标看着轻微一顿。
                            // 改成固定节拍槽位（每 16.67ms 一格）后上屏间隔恒定。
                            // 代价：整幅画面（含鼠标）平均晚约 8ms、最坏 16.7ms。
                            if (RENDER_PACE_NS <= 0) {
                                mc.releaseOutputBuffer(index, true);
                            } else {
                                long now = System.nanoTime();
                                if (nextRenderNs == 0 || nextRenderNs < now - 2 * RENDER_PACE_NS) {
                                    nextRenderNs = now + PACE_LEAD_NS;      // 首帧/断流/落后过多：重锚
                                }
                                long ns = nextRenderNs;
                                nextRenderNs += RENDER_PACE_NS;
                                mc.releaseOutputBuffer(index, Math.max(ns, now));
                            }
                        } catch (Throwable e) {
                            pipe.sendEmptyMessage(MSG_ERR);
                        }
                    }
                    @Override public void onError(MediaCodec mc, MediaCodec.CodecException e) {
                        Log.w(TAG, "codec error", e);
                        Logx.e("codec error: " + e);
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
        csdSps = null;                                           // decoder gone: force reconfig
        csdPps = null;                                           // when the next SPS pair lands
        inIdx.clear();
        auQ.clear();
        width = 0; height = 0;
        framesFed.set(0);
        prevFrames = 0;
        fps = 0;
        nextRenderNs = 0;                                       // 解码器重配后节拍要重锚
        asmReset = true;                                       // rx resets assemblers next packet
        postStatus();
    }

    // ---------------- audio thread (ADTS -> AAC-LC decode -> AudioTrack, M3) ----------------

    private static final int[] ADTS_RATES = {96000, 88200, 64000, 48000, 44100,
            32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350};

    private void onAud(Message m) {
        if (m.what != MSG_AUDIO) return;
        aPend.decrementAndGet();
        byte[] adts = (byte[]) m.obj;
        int fi = (adts[2] >> 2) & 0xF;                         // sampling_freq_index
        int ch = ((adts[2] & 1) << 2) | ((adts[3] >> 6) & 3);  // channel_configuration
        int rate = fi < 13 ? ADTS_RATES[fi] : 44100;
        if (ch < 1 || ch > 2) ch = 2;
        int hdr = (adts[1] & 1) != 0 ? 7 : 9;                  // protection_bit: 1 = no CRC
        if (acodec == null || rate != aRate || ch != aCh) configureAudio(rate, ch, fi);
        if (acodec != null && adts.length > hdr) decodeAudio(adts, hdr);
    }

    private void configureAudio(int rate, int ch, int fi) {
        releaseAudio();
        try {
            // AudioSpecificConfig v1: AOT=2 (AAC-LC) | freq idx | chan cfg
            byte[] asc = new byte[] { (byte) ((2 << 3) | (fi >> 1)),
                    (byte) (((fi & 1) << 7) | (ch << 3)) };
            MediaFormat f = MediaFormat.createAudioFormat("audio/mp4a-latm", rate, ch);
            f.setInteger(MediaFormat.KEY_CHANNEL_COUNT, ch);
            f.setByteBuffer("csd-0", ByteBuffer.wrap(asc));
            MediaCodec c = MediaCodec.createDecoderByType("audio/mp4a-latm");
            c.configure(f, null, null, 0);
            c.start();
            int chMask = ch == 1 ? AudioFormat.CHANNEL_OUT_MONO : AudioFormat.CHANNEL_OUT_STEREO;
            int min = AudioTrack.getMinBufferSize(rate, chMask, AudioFormat.ENCODING_PCM_16BIT);
            if (min <= 0) min = 8192;
            // min*4 (≈0.8s TV buffer) + blocking-write pacing = the reported ~3s audio
            // lag. Real-time stream: keep the hardware buffer small; underruns are
            // preferable to latency (M3.1 resync will cover rare gaps).
            AudioTrack t = new AudioTrack(AudioManager.STREAM_MUSIC, rate, chMask,
                    AudioFormat.ENCODING_PCM_16BIT, min + min / 2, AudioTrack.MODE_STREAM);
            t.play();
            acodec = c; atrack = t; aRate = rate; aCh = ch;
            aWrittenBytes = 0;
        } catch (Throwable e) {
            releaseAudio();
        }
        postStatus();
    }

    /** One ADTS frame in, 1024-sample PCM block out; the blocking AudioTrack.write
     *  paces this thread, so no PTS scheduling is needed yet (M3.1 will add A/V sync). */
    private void decodeAudio(byte[] adts, int hdr) {
        try {
            // non-blocking: blocking 10ms per frame on a decoder hiccup backs up the
            // handler queue and adds seconds of audio lag (reported ~3s).
            int idx = acodec.dequeueInputBuffer(0);
            if (idx >= 0) {
                ByteBuffer b = acodec.getInputBuffer(idx);
                if (b != null) {
                    b.clear();
                    b.put(adts, hdr, adts.length - hdr);       // ADTS header stripped, ASC in csd-0
                    acodec.queueInputBuffer(idx, 0, adts.length - hdr, 0, 0);
                }
            }
            MediaCodec.BufferInfo bi = new MediaCodec.BufferInfo();
            int oi;
            while ((oi = acodec.dequeueOutputBuffer(bi, 0)) >= 0) {
                if (bi.size > 0 && atrack != null) {
                    ByteBuffer ob = acodec.getOutputBuffer(oi);
                    if (ob != null) {
                        byte[] pcm = new byte[bi.size];
                        ob.position(bi.offset);
                        ob.get(pcm, 0, bi.size);
                        int w = atrack.write(pcm, 0, bi.size);
                        if (w > 0) aWrittenBytes += w;
                    }
                }
                acodec.releaseOutputBuffer(oi, false);
                audioFed.incrementAndGet();
            }
        } catch (Throwable e) {
            releaseAudio();
        }
    }

    private void releaseAudio() {
        AudioTrack t = atrack; atrack = null;
        aWrittenBytes = 0;        MediaCodec c = acodec; acodec = null;
        aRate = 0; aCh = 0;
        if (t != null) {
            try { t.stop(); } catch (Throwable ignored) { }
            try { t.release(); } catch (Throwable ignored) { }
        }
        if (c != null) {
            try { c.stop(); } catch (Throwable ignored) { }
            try { c.release(); } catch (Throwable ignored) { }
        }
    }

    /** 已写入但还没播出的 PCM 毫秒数（= AudioTrack 缓冲延迟）。 */
    private int audioBufferedMs() {
        AudioTrack t = atrack;
        int rate = aRate, ch = aCh;
        if (t == null || rate <= 0 || ch <= 0) return -1;
        try {
            long playedFrames = t.getPlaybackHeadPosition() & 0xFFFFFFFFL;
            long queued = aWrittenBytes - playedFrames * ch * 2L;
            if (queued < 0) queued = 0;
            return (int) (queued * 1000L / ((long) rate * ch * 2L));
        } catch (Throwable e) {
            return -1;
        }
    }

    /** AudioTrack 估计的硬件输出延迟（毫秒）。getLatency() 在新 SDK 不可用，改用时间戳估算。 */
    private int audioOutputLatencyMs() {
        AudioTrack t = atrack;
        int rate = aRate;
        if (t == null || rate <= 0) return -1;
        try {
            android.media.AudioTimestamp ts = new android.media.AudioTimestamp();
            if (t.getTimestamp(ts)) {
                long played = t.getPlaybackHeadPosition() & 0xFFFFFFFFL;
                long framesAhead = played - ts.framePosition;
                if (framesAhead < 0) framesAhead = 0;
                return (int) (framesAhead * 1000L / rate);
            }
        } catch (Throwable ignored) { }
        return -1;
    }

    /** 兜底：心跳还在但 3 秒没有画面（PC 锁屏/捕获暂停/编码卡住）→ 让电视显示暂停遮罩。 */
    private void checkPauseFallback() {
        long now = System.currentTimeMillis();
        boolean alive = lastRxWall > 0 && (now - lastRxWall) < 3000;
        boolean stalled = alive && framesFed.get() > 0 && lastFrameWall > 0 && (now - lastFrameWall) > 3000;
        if (stalled && !pauseShown) {
            pauseShown = true;
            final String ev = "pz";
            main.post(new Runnable() { @Override public void run() { sink.onEvent(ev); } });
        } else if (!stalled && pauseShown) {
            pauseShown = false;
            final String ev = "rs";
            main.post(new Runnable() { @Override public void run() { sink.onEvent(ev); } });
        }
    }

    private void tick() {
        checkPauseFallback();
        long n = framesFed.get();
        fps = n - prevFrames;                                  // 1s cadence
        prevFrames = n;
        if (Logx.isEnabled() && (++tickN % 5) == 0) {
            Logx.i("stat fps=" + fps + " fed=" + framesFed.get() + " lost=" + lostSeq.get()
                    + " dropAu=" + droppedAu.get() + " idr=" + fedIdr.get()
                    + " aud=" + audioFed.get() + " adrop=" + audioDrop.get()
                + " host=" + (pcName.isEmpty() ? "-" : pcName)
                + " pport=" + (pcPort.isEmpty() ? "-" : pcPort)
                    + " res=" + (width > 0 ? width + "x" + height : "-"));
        }
        boolean streaming = lastRxMs != 0
                && SystemClock.elapsedRealtime() - lastRxMs < IDLE_MS;
        // 空闲超时释放解码器；但「主动断开」后要留 3.5 秒冻结最后一帧。
        // 冻结期结束后把 peer 也清掉，否则电视端会一直显示"正在连接…"（源设备已经关掉了）。
        if (!streaming && SystemClock.elapsedRealtime() > freezeUntil) {
            if (configured) releaseCodec();
            if (peer != null && !peer.equals("-")) { peer = "-"; postStatus(); }
        }
        Handler h = audh;                                      // audio idles independently
        if (h != null && lastAudioMs != 0
                && SystemClock.elapsedRealtime() - lastAudioMs > IDLE_MS) {
            h.post(new Runnable() {
                @Override public void run() { releaseAudio(); postStatus(); }
            });
        }
        postStatus();
        pipe.sendEmptyMessageDelayed(MSG_TICK, 1000);
    }

    // ---------------- status line ----------------

    /** 向界面投递一次性事件（lk/ul/sw/pz/rs/hi/bye）。 */
    private void emitEvent(final String ev) {
        main.post(new Runnable() { @Override public void run() { sink.onEvent(ev); } });
    }

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
                + " idr=" + fedIdr.get()
                + " meta=" + metaCount.get() + " res=" + res
                + " aud=" + audioFed.get() + " adrop=" + audioDrop.get()
                + " host=" + (pcName.isEmpty() ? "-" : pcName)
                + " pport=" + (pcPort.isEmpty() ? "-" : pcPort)
                + (waitingIdr ? " waitIDR" : "")
                + (Logx.isEnabled() ? " LOG" : "");
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
        return (sps == null && pps == null) ? null : new Cfg(sps, pps);
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
