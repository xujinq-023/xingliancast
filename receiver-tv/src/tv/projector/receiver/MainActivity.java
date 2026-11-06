package tv.projector.receiver;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.PowerManager;
import android.util.TypedValue;
import android.view.KeyEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowManager;
import android.widget.FrameLayout;
import android.widget.TextView;

/** Fullscreen immersive SurfaceView + status overlay; all media work lives in RxEngine. */
public class MainActivity extends Activity {

    /** PowerManager.SCREEN_KEEP_ON_WAKE_LOCK is @hide; the int value is public knowledge. */
    private static final int SCREEN_KEEP_ON_WAKE_LOCK = 0x00000007;

    private SurfaceView surfaceView;
    private TextView statusView;
    private RxEngine engine;
    private PowerManager.WakeLock wakeLock;
    private boolean wakeLockOn;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);

        engine = new RxEngine(new RxEngine.UiSink() {
            @Override public void onStatus(String text) {
                if (statusView != null) statusView.setText(text);
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

        statusView = new TextView(this);
        statusView.setTypeface(Typeface.MONOSPACE);
        statusView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 13f);
        statusView.setTextColor(Color.WHITE);
        statusView.setBackgroundColor(0xB0000000);
        int pad = (int) (8 * getResources().getDisplayMetrics().density);
        statusView.setPadding(pad, pad, pad, pad);
        FrameLayout.LayoutParams lp = new FrameLayout.LayoutParams(-2, -2);
        int m = (int) (12 * getResources().getDisplayMetrics().density);
        lp.setMargins(m, m, 0, 0);
        root.addView(statusView, lp);

        setContentView(root);
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

    /** BACK (remote EXIT) quits the receiver. */
    @Override
    public boolean onKeyDown(int keyCode, KeyEvent event) {
        if (keyCode == KeyEvent.KEYCODE_BACK) {
            finish();
            return true;
        }
        return super.onKeyDown(keyCode, event);
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
