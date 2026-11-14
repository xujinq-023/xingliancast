package tv.projector.receiver;

import android.app.Activity;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.PointerIcon;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.TextView;
import android.widget.Toast;

import java.net.Inet4Address;
import java.net.InetAddress;
import java.net.NetworkInterface;
import java.util.Collections;

/**
 * TV receiver UI:
 *   - idle waiting screen (logo + name + device/IP hint) shown while no stream is running
 *   - fullscreen SurfaceView showing the cast video
 *   - tiny diagnostic overlay (top-left) kept for troubleshooting
 */
public class MainActivity extends Activity {

    /** PowerManager.SCREEN_KEEP_ON_WAKE_LOCK is @hide; the int value is public knowledge. */
    private static final int SCREEN_KEEP_ON_WAKE_LOCK = 0x00000007;

    private SurfaceView surfaceView;
    private TextView statusView;
    private View idleView;
    private TextView infoView;
    private TextView subView;
    private TextView tipView;
    private boolean wasStreaming;
    private boolean locked;          // 电脑端已锁屏（显示锁屏页，连接保持）
    private String pcHost = "", pcIp = "", pcPort = "", tvInfo = "";
    private boolean streamingNow;
    private String lastKey;
    private TextView bannerView;      // 断线时顶部提示条

    private TextView infoBar;         // 投屏中的小信息条（分辨率/fps）
    private int overlayMode;          // 0=隐藏 1=小信息条 2=诊断浮层
    private final Runnable bannerTimeout = new Runnable() {
        @Override public void run() {
            if (bannerView != null) bannerView.setVisibility(View.GONE);
            wasStreaming = false;
            lastKey = null;                    // 让下一次状态回调重新计算
            setSub("等待电脑投屏…", 0xFFB9B3DA);
            showIdle();                        // 3 秒后回到等待页
        }
    };
    private boolean errorState;
    // 电脑端主动点「停止投屏」会发 bye：这种情况文案是「电脑端已停止投屏」，
    // 与"掉线/被踢"的「连接已断开，等待重连…」要区分开。
    private boolean gracefulStop;
    // 提示条超时：只负责把提示条收起来，绝不能动画面（旧版用的是 bannerTimeout，
    // 那个 Runnable 会强制 showIdle()，导致「另一台电脑尝试投屏」提示 3 秒后画面被切回等待页）
    private final Runnable bannerHide = new Runnable() {
        @Override public void run() {
            if (bannerView != null) bannerView.setVisibility(View.GONE);
        }
    };
    private final Handler uiHandler = new Handler(Looper.getMainLooper());
    private final Runnable idleReset = new Runnable() {
        @Override public void run() {
            // 断开后 10 秒没有任何数据 → 回到"等待电脑投屏"
            if (idleView != null && idleView.getVisibility() == View.VISIBLE && !streamingNow) {
                setSub("等待电脑投屏…", 0xFFB9B3DA);
                if (tipView != null) {
                    tipView.setText("在电脑上打开「星联投屏」，选择本机后点击「开始投屏」");
                    tipView.setTextColor(0xFF6E6A90);
                }
            }
        }
    };
    private RxEngine engine;
    private PowerManager.WakeLock wakeLock;
    private boolean wakeLockOn;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        Logx.init(this);
        // 记录未捕获异常到文件（adb pull 可读），避免"电视闪一下等待页"却查不到原因
        try {
            final Thread.UncaughtExceptionHandler prev = Thread.getDefaultUncaughtExceptionHandler();
            Thread.setDefaultUncaughtExceptionHandler(new Thread.UncaughtExceptionHandler() {
                @Override public void uncaughtException(Thread t, Throwable e) {
                    try { Logx.e("FATAL in " + t.getName() + ": " + android.util.Log.getStackTraceString(e)); } catch (Throwable ignored) { }
                    if (prev != null) prev.uncaughtException(t, e);
                }
            });
        } catch (Throwable ignored) { }

        engine = new RxEngine(new RxEngine.UiSink() {
            @Override public void onStatus(String text) {
                if (statusView != null) statusView.setText(text);
                updateIdleVisibility(text);
            }

            @Override public void onEvent(String ev) {
                if ("hi".equals(ev)) { locked = false; gracefulStop = false; lastKey = null; }
                else if ("bye".equals(ev)) { locked = false; gracefulStop = true; lastKey = null; }
                else if ("lk".equals(ev)) { locked = true; lastKey = null; renderLockPage(); }
                else if ("ul".equals(ev)) { locked = false; lastKey = null; }
                else if ("sw".equals(ev)) showBanner("被投显示器已断开，已切换到主显示器");
                else if ("b2".equals(ev)) showBanner("另一台电脑尝试投屏，已拒绝");
                else if ("pz".equals(ev) && !locked) { locked = true; lastKey = null; renderLockPage(); }
                else if ("rs".equals(ev) && locked) { locked = false; lastKey = null; }
            }
        });

        FrameLayout root = new FrameLayout(this);
        root.setBackgroundColor(Color.BLACK);

        surfaceView = new SurfaceView(this);
        surfaceView.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) {
                engine.setSurface(holder.getSurface());
            }
            @Override public void surfaceChanged(SurfaceHolder holder, int format,
                                                 int width, int height) {
                engine.setSurface(holder.getSurface());
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                engine.setSurface(null);
            }
        });
        root.addView(surfaceView, new FrameLayout.LayoutParams(-1, -1));
        hideSystemPointer(surfaceView);
        hideSystemPointer(root);

        // ---------------- idle / waiting screen ----------------
        FrameLayout idle = new FrameLayout(this);
        GradientDrawable bg = new GradientDrawable(
                GradientDrawable.Orientation.TL_BR, new int[] { 0xFF120E24, 0xFF241A4E });
        idle.setBackground(bg);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        col.setGravity(Gravity.CENTER_HORIZONTAL);
        FrameLayout.LayoutParams colLp = new FrameLayout.LayoutParams(-1, -2);
        colLp.gravity = Gravity.CENTER;
        idle.addView(col, colLp);

        ImageView logo = new ImageView(this);
        int logoId = getResources().getIdentifier("logo", "drawable", getPackageName());
        if (logoId != 0) {
            Bitmap bmp = BitmapFactory.decodeResource(getResources(), logoId);
            logo.setImageBitmap(bmp);
        }
        int side = dp(150);
        LinearLayout.LayoutParams logoLp = new LinearLayout.LayoutParams(side, side);
        logoLp.bottomMargin = dp(26);
        col.addView(logo, logoLp);

        TextView name = new TextView(this);
        name.setText("星联投屏");
        name.setTextColor(0xFFFFFFFF);
        name.setTextSize(TypedValue.COMPLEX_UNIT_SP, 44f);
        name.setTypeface(Typeface.DEFAULT_BOLD);
        name.setGravity(Gravity.CENTER);
        col.addView(name);

        subView = new TextView(this);
        TextView sub = subView;
        sub.setText("等待电脑投屏…");
        sub.setTextColor(0xFFB9B3DA);
        sub.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f);
        sub.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams subLp = new LinearLayout.LayoutParams(-2, -2);
        subLp.topMargin = dp(10);
        col.addView(sub, subLp);

        infoView = new TextView(this);
        infoView.setTextColor(0xFF7C78A0);
        infoView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f);
        infoView.setGravity(Gravity.START);        // 行内左对齐（整块由 layout gravity 居中）
        LinearLayout.LayoutParams infoLp = new LinearLayout.LayoutParams(-2, -2);
        infoLp.topMargin = dp(22);
        infoLp.gravity = Gravity.CENTER_HORIZONTAL;
        col.addView(infoView, infoLp);

        tipView = new TextView(this);
        TextView tip = tipView;
        tip.setText("在电脑上打开「星联投屏」，选择本机后点击「开始投屏」");
        tip.setTextColor(0xFF6E6A90);
        tip.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16f);
        tip.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams tipLp = new LinearLayout.LayoutParams(-2, -2);
        tipLp.topMargin = dp(56);
        col.addView(tip, tipLp);

        TextView keys = new TextView(this);
        keys.setText("按返回键退出   ·   按 MENU 键切换状态显示");
        keys.setTextColor(0xFF4E4A70);
        keys.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        keys.setGravity(Gravity.CENTER);
        LinearLayout.LayoutParams keysLp = new LinearLayout.LayoutParams(-2, -2);
        keysLp.topMargin = dp(26);
        col.addView(keys, keysLp);

        TextView ver = new TextView(this);
        ver.setText("星联投屏 TV  v1.0.12");
        ver.setTextColor(0xFF4E4A70);
        ver.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        FrameLayout.LayoutParams verLp = new FrameLayout.LayoutParams(-2, -2);
        verLp.gravity = Gravity.BOTTOM | Gravity.RIGHT;
        verLp.setMargins(0, 0, dp(24), dp(18));
        idle.addView(ver, verLp);

        root.addView(idle, new FrameLayout.LayoutParams(-1, -1));
        idleView = idle;

        statusView = new TextView(this);
        statusView.setTypeface(Typeface.MONOSPACE);
        statusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 12f);
        statusView.setTextColor(0xCCFFFFFF);
        statusView.setBackgroundColor(0x90000000);
        int pad = dp(6);
        statusView.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(-2, -2);
        int m = dp(10);
        slp.setMargins(m, m, 0, 0);
        statusView.setOnLongClickListener(new View.OnLongClickListener() {
            @Override public boolean onLongClick(View v) {
                toggleLog();
                return true;
            }
        });
        buildLockPanel(root);

        // 断线提示条：冻结最后一帧 + 顶部半透明提示，3 秒后回到等待页
        bannerView = new TextView(this);
        bannerView.setText("连接已断开，等待重连…");
        bannerView.setTextColor(0xFFFFFFFF);
        bannerView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 17f);
        bannerView.setGravity(Gravity.CENTER);
        bannerView.setBackgroundColor(0xC0201A3A);
        bannerView.setPadding(0, dp(10), 0, dp(10));
        bannerView.setVisibility(View.GONE);
        FrameLayout.LayoutParams blp = new FrameLayout.LayoutParams(-1, -2);
        blp.gravity = Gravity.TOP;
        root.addView(bannerView, blp);

        // 小信息条：投屏中显示分辨率/帧率（默认隐藏，MENU 键循环切换）
        infoBar = new TextView(this);
        infoBar.setTextColor(0xCCFFFFFF);
        infoBar.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14f);
        infoBar.setBackgroundColor(0x70000000);
        infoBar.setPadding(dp(10), dp(5), dp(10), dp(5));
        infoBar.setVisibility(View.GONE);
        FrameLayout.LayoutParams ilp = new FrameLayout.LayoutParams(-2, -2);
        ilp.gravity = Gravity.TOP | Gravity.RIGHT;
        int im = dp(14);
        ilp.setMargins(0, im, im, 0);
        root.addView(infoBar, ilp);

        statusView.setVisibility(View.GONE);
        root.addView(statusView, slp);

        setContentView(root);
        refreshInfo();
    }

    /** Android draws its own mouse pointer over the SurfaceView; the PC cursor is already in the
     *  video, so the extra pointer shows up as a black blob. Hide it. */
    private void hideSystemPointer(View v) {
        try { v.setPointerIcon(PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL)); }
        catch (Throwable ignored) { }
    }

    private void refreshInfo() {
        if (infoView == null) return;
        String ip = localIp();
        String model = Build.MODEL == null ? "TV" : Build.MODEL;
        tvInfo = model + "   " + (ip.length() > 0 ? ip : "无网络") + ":45690";
        infoView.setText(tvInfo);
    }

    private String localIp() {
        try {
            for (NetworkInterface ni : Collections.list(NetworkInterface.getNetworkInterfaces())) {
                if (!ni.isUp() || ni.isLoopback()) continue;
                for (InetAddress a : Collections.list(ni.getInetAddresses())) {
                    if (a instanceof Inet4Address && !a.isLoopbackAddress()) return a.getHostAddress();
                }
            }
        } catch (Throwable ignored) { }
        return "";
    }

    /** 根据引擎状态文本切换界面：投屏中隐藏等待页；未投屏时显示 等待/正在连接/已断开/错误。
     *  引擎每秒都会回调状态，这里用状态键去重，避免重复动画和重置定时器被无限推迟。 */
    private void updateIdleVisibility(String text) {
        if (idleView == null || text == null) return;
        boolean streaming = text.startsWith("READY");
        boolean error = text.startsWith("ERROR");
        String peer = null;
        int i = text.indexOf("pc=");
        if (i >= 0) {
            int j = i + 3;
            while (j < text.length() && !Character.isWhitespace(text.charAt(j))) j++;   // 截到空白，避免吃进 fps=
            peer = text.substring(i + 3, j).trim();
            if (peer.isEmpty() || peer.equals("-") || peer.equals("?")) peer = null;
        }
        // 解析源设备信息（host=计算机名，pc=IP）
        int hi = text.indexOf("host=");
        if (hi >= 0) {
            int he = text.indexOf(' ', hi);
            String h = he < 0 ? text.substring(hi + 5) : text.substring(hi + 5, he);
            if (!h.equals("-")) pcHost = h.trim();
        }
        if (peer != null) pcIp = peer;
        int pi2 = text.indexOf("pport=");
        if (pi2 >= 0) {
            int pj = pi2 + 6;
            while (pj < text.length() && !Character.isWhitespace(text.charAt(pj))) pj++;
            String pv = text.substring(pi2 + 6, pj).trim();
            if (!pv.equals("-")) pcPort = pv;
        }

        streamingNow = streaming;
        if (streaming) wasStreaming = true;
        if (locked) { renderLockPage(); return; }   // 锁屏页优先，保持连接）

        // 断线判断看"是否投屏过"，而不是 peer 是否存在（bye 会把 peer 置为 "-"）
        String key = streaming ? "S" : error ? "E" : wasStreaming ? "D" : (peer != null ? "C" : "I");
        if (key.equals(lastKey)) return;
        lastKey = key;

        if (streaming) {
            uiHandler.removeCallbacks(idleReset);
            uiHandler.removeCallbacks(bannerTimeout);
            uiHandler.removeCallbacks(bannerHide);
            if (bannerView != null) bannerView.setVisibility(View.GONE);
            hideIdle();
            updateInfoBar(text);
            return;
        }
        if (infoBar != null) infoBar.setVisibility(View.GONE);
        if ("D".equals(key)) {
            // 断线：保留最后一帧，顶部提示条 3 秒后再回到等待页
            uiHandler.removeCallbacks(idleReset);
            setSub(gracefulStop ? "电脑端已停止投屏" : "连接已断开，等待重连…", 0xFF9FE3C8);
            tipView.setText(peer != null ? "源设备 " + peer : "");
            tipView.setTextColor(0xFF6E6A90);
            bannerView.setText(gracefulStop ? "电脑端已停止投屏" : "连接已断开，等待重连…");
            bannerView.setVisibility(View.VISIBLE);
            uiHandler.removeCallbacks(bannerTimeout);
            uiHandler.postDelayed(bannerTimeout, 3000);
            return;
        }
        if (bannerView != null) bannerView.setVisibility(View.GONE);
        showIdle();
        uiHandler.removeCallbacks(idleReset);
        uiHandler.postDelayed(idleReset, 10000);
        switch (key) {
            case "E":
                setSub("接入失败", 0xFFE5677A);
                tipView.setText(text.replace('\n', ' '));
                tipView.setTextColor(0xFFB98A96);
                break;
            case "D":
                // 冻结最后一帧 + 顶部提示条 3 秒，再回到等待页
                setSub(gracefulStop ? "电脑端已停止投屏" : "连接已断开，等待重连…", 0xFF9FE3C8);
                tipView.setText(peer != null ? "源设备 " + peer : "");
                tipView.setTextColor(0xFF6E6A90);
                bannerView.setVisibility(View.VISIBLE);
                uiHandler.removeCallbacks(bannerTimeout);
                uiHandler.postDelayed(bannerTimeout, 3000);
                break;
            case "C":
                setSub("正在连接…", 0xFF9FE3C8);
                tipView.setText("源设备 " + peer);
                tipView.setTextColor(0xFF6E6A90);
                break;
            default:
                setSub(localIp().isEmpty() ? "请先连接网络" : "等待电脑投屏…", 0xFFB9B3DA);
                tipView.setText("在电脑上打开「星联投屏」，选择本机后点击「开始投屏」");
                tipView.setTextColor(0xFF6E6A90);
                wasStreaming = false;
                break;
        }
    }
    // ---------------- 暂停遮罩（模糊化） ----------------
    private FrameLayout lockPanel;
    private ImageView lockImage;
    private TextView lockText;

    /** 抓取当前 SurfaceView 画面 → 强降采样 → 拉回全屏，得到"磨砂"效果（SurfaceView 无法直接用 View 糊）。 */
    private void captureBlurredFrame() {
        if (surfaceView == null || lockImage == null) return;
        final int w = surfaceView.getWidth(), h = surfaceView.getHeight();
        if (w <= 0 || h <= 0) return;
        try {
            final Bitmap full = Bitmap.createBitmap(w, h, Bitmap.Config.ARGB_8888);
            android.view.PixelCopy.request(surfaceView, full, new android.view.PixelCopy.OnPixelCopyFinishedListener() {
                @Override public void onPixelCopyFinished(int result) {
                    if (result != android.view.PixelCopy.SUCCESS) { full.recycle(); return; }
                    try {
                        // 1/32 降采样再放大 = 强模糊（文字细节彻底不可辨，保护隐私）
                        int sw = Math.max(6, w / 32), sh = Math.max(4, h / 32);
                        Bitmap small = Bitmap.createScaledBitmap(full, sw, sh, true);
                        // 小图上再做两轮缩放，抹掉像素块感
                        Bitmap s2 = Bitmap.createScaledBitmap(small, Math.max(4, sw / 2), Math.max(3, sh / 2), true);
                        Bitmap blur = Bitmap.createScaledBitmap(s2, w, h, true);
                        lockImage.setImageBitmap(blur);
                        lockImage.setVisibility(View.VISIBLE);
                        full.recycle();
                    } catch (Throwable t) {
                        full.recycle();
                    }
                }
            }, uiHandler);
        } catch (Throwable ignored) {
            // 部分设备/分辨率不支持 PixelCopy：退化为纯色底（仍有隐私效果）
        }
    }

    private void buildLockPanel(FrameLayout root) {
        lockPanel = new FrameLayout(this);
        lockPanel.setVisibility(View.GONE);

        lockImage = new ImageView(this);
        lockImage.setScaleType(ImageView.ScaleType.CENTER_CROP);
        lockImage.setVisibility(View.GONE);
        lockPanel.addView(lockImage, new FrameLayout.LayoutParams(-1, -1));

        // 轻微压暗，保证文字可读（不追求很黑，模糊本身已在保护隐私）
        View scrim = new View(this);
        scrim.setBackgroundColor(0xB46E6E7A);   // 灰色遮罩（70% 深中灰）：模糊 + 厚灰纱
        lockPanel.addView(scrim, new FrameLayout.LayoutParams(-1, -1));

        lockText = new TextView(this);
        lockText.setTextColor(0xFFFFFFFF);
        lockText.setTextSize(TypedValue.COMPLEX_UNIT_SP, 22f);
        lockText.setGravity(Gravity.CENTER);
        lockText.setShadowLayer(6f, 0f, 2f, 0xCC000000);
        lockPanel.addView(lockText, new FrameLayout.LayoutParams(-1, -1));

        root.addView(lockPanel, new FrameLayout.LayoutParams(-1, -1));
    }

    /** 显示/隐藏暂停遮罩；text 为 null 表示隐藏。blur=true 时抓当前画面做模糊底。 */
    private void showOverlay(String text, boolean show) {
        if (lockPanel == null) return;
        if (!show || text == null) {
            lockPanel.setVisibility(View.GONE);
            if (lockImage != null) { lockImage.setImageBitmap(null); lockImage.setVisibility(View.GONE); }
            return;
        }
        lockText.setText(text);
        lockPanel.setVisibility(View.VISIBLE);
        lockPanel.bringToFront();
        if (text != null && text.startsWith("被投显示器")) {
            uiHandler.removeCallbacks(overlayTimeout);
            uiHandler.postDelayed(overlayTimeout, 3000);
        }
    }

    private final Runnable overlayTimeout = new Runnable() {
        @Override public void run() { showOverlay(null, false); }
    };
    /** 投屏中：右上角显示分辨率 + 实际帧率（MENU 键切换显示）。 */
    private void updateInfoBar(String text) {
        if (infoBar == null) return;
        if (overlayMode != 1) { infoBar.setVisibility(View.GONE); return; }
        String res = "-", fps = "-";
        int r = text.indexOf("res=");
        if (r >= 0) {
            int e = text.indexOf(' ', r);
            String v = e < 0 ? text.substring(r + 4) : text.substring(r + 4, e);
            res = (v.startsWith("-")) ? "—" : v;
        }
        int f = text.indexOf("fps=");
        if (f >= 0) {
            int e = text.indexOf(' ', f);
            String v = e < 0 ? text.substring(f + 4) : text.substring(f + 4, e);
            try { fps = String.format(java.util.Locale.US, "%.0f", Double.parseDouble(v)); } catch (Throwable ignored) { }
        }
        infoBar.setText(res + "   " + fps + " fps" + "\n" + deviceInfoText());
        infoBar.setGravity(Gravity.START);
        infoBar.setVisibility(View.VISIBLE);
    }

    /** 电脑锁屏时的整屏页面：复用等待页样式，状态改为"电脑已锁屏"，展示源设备名与 IP。 */
    private void renderLockPage() {
        if (idleView == null) return;
        showIdle();
        uiHandler.removeCallbacks(idleReset);
        setSub("电脑已锁屏", 0xFF9FE3C8);
        if (infoView != null) infoView.setText(deviceInfoText());
        if (tipView != null) {
            tipView.setText("解锁电脑后将自动恢复投屏");
            tipView.setTextColor(0xFF6E6A90);
        }
        if (bannerView != null) bannerView.setVisibility(View.GONE);
        if (infoBar != null) infoBar.setVisibility(View.GONE);
    }

    /** TV / PC 两行设备信息（左对齐，冒号两侧留空格）。 */
    private String deviceInfoText() {
        String pc = pcHost.isEmpty() ? "未连接" : pcHost;
        if (!pcIp.isEmpty()) pc += "   " + pcIp + (pcPort.isEmpty() ? "" : ":" + pcPort);
        return "TV  →  " + (tvInfo.isEmpty() ? "—" : tvInfo) + "\n" + "PC  →  " + pc;
    }

    /** 顶部提示条（3 秒）：用于"已切换到主显示器"这类路过信息。 */
    private void showBanner(String text) {
        if (bannerView == null) return;
        bannerView.setText(text);
        bannerView.setVisibility(View.VISIBLE);
        uiHandler.removeCallbacks(bannerHide);
        uiHandler.postDelayed(bannerHide, 3000);
    }

    private void setSub(String s, int color) {
        if (subView == null) return;
        subView.setText(s);
        subView.setTextColor(color);
    }

    private void showIdle() {
        if (idleView.getVisibility() == View.VISIBLE) return;
        refreshInfo();
        idleView.setAlpha(0f);
        idleView.setVisibility(View.VISIBLE);
        idleView.animate().alpha(1f).setDuration(240).start();
    }

    private void hideIdle() {
        if (idleView.getVisibility() != View.VISIBLE) return;
        idleView.animate().alpha(0f).setDuration(200).withEndAction(new Runnable() {
            @Override public void run() { idleView.setVisibility(View.GONE); }
        }).start();
    }

    private int dp(int v) {
        return (int) (v * getResources().getDisplayMetrics().density);
    }

    @Override
    protected void onResume() {
        super.onResume();
        immersive();
        try {
            if (wakeLock == null) {
                PowerManager pm = (PowerManager) getSystemService(POWER_SERVICE);
                wakeLock = pm.newWakeLock(SCREEN_KEEP_ON_WAKE_LOCK, "ProjectorRX:screen");
            }
            if (!wakeLockOn) { wakeLock.acquire(); wakeLockOn = true; }
        } catch (Throwable ignored) {
            // screen keep-on still guaranteed by FLAG_KEEP_SCREEN_ON window flag
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) immersive();
    }

    @Override
    protected void onPause() {
        super.onPause();
        try {
            if (wakeLock != null && wakeLockOn) { wakeLock.release(); wakeLockOn = false; }
        } catch (Throwable ignored) { }
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (engine != null) engine.stop();
    }

    /** BACK (remote EXIT) quits the receiver; MENU toggles the optional file log. */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            finish();
            return true;
        }
        if (keyCode == KeyEvent.KEYCODE_MENU) {
            // MENU：隐藏 → 小信息条 → 诊断浮层 → 隐藏
            overlayMode = (overlayMode + 1) % 3;
            statusView.setVisibility(overlayMode == 2 ? View.VISIBLE : View.GONE);
            infoBar.setVisibility(overlayMode == 1 ? View.VISIBLE : View.GONE);
            if (overlayMode == 1) updateInfoBar(statusView.getText().toString());
            Toast.makeText(this,
                    overlayMode == 0 ? "状态显示：关闭" : overlayMode == 1 ? "状态显示：分辨率/帧率" : "状态显示：诊断信息",
                    Toast.LENGTH_SHORT).show();
            return true;
        }
        return super.onKeyDown(keyCode, event);
    }

    private void toggleLog() {
        boolean on = !Logx.isEnabled();
        Logx.setEnabled(on);
        Toast.makeText(this, on ? ("Logging ON\n" + Logx.path()) : "Logging OFF",
                Toast.LENGTH_LONG).show();
    }

    @SuppressWarnings("deprecation")
    private void immersive() {
        getWindow().getDecorView().setSystemUiVisibility(
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                        | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                        | View.SYSTEM_UI_FLAG_FULLSCREEN
                        | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY);
    }
}
