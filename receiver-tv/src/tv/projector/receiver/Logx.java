package tv.projector.receiver;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/**
 * Optional file logger for the TV receiver. OFF by default: the UI switch calls
 * {@link #setEnabled(boolean)}.
 *
 * <p>Files live in the app's external files dir ({@code Android/data/<pkg>/files/logs}),
 * so they can be pulled with {@code adb pull} without root. Rotation is by size:
 * when receiver.log exceeds {@link #setMaxMb} it is renamed to receiver.log.1 (the
 * previous .1 is overwritten), so disk use is bounded by 2 × max.
 */
final class Logx {
    private static final Object L = new Object();
    private static final SimpleDateFormat FMT =
            new SimpleDateFormat("yyyy-MM-dd HH:mm:ss.SSS", Locale.US);

    private static File dir;
    private static File file;
    private static long maxBytes = 5L << 20;      // default 5 MB (UI: 5..100)
    private static volatile boolean enabled = false;

    private Logx() { }

    static void init(Context c) {
        try {
            File base = c.getExternalFilesDir(null);
            if (base == null) base = c.getFilesDir();
            dir = new File(base, "logs");
            if (!dir.exists()) dir.mkdirs();
            file = new File(dir, "receiver.log");
        } catch (Throwable e) {
            Log.w("pjs-log", "init failed", e);
        }
    }

    static boolean isEnabled() { return enabled; }

    static void setEnabled(boolean on) {
        enabled = on;
        if (on) i("file logging enabled (max " + (maxBytes >> 20) + " MB, " + path() + ")");
    }

    /** UI setting: 5..100 MB (clamped). */
    static void setMaxMb(int mb) {
        int v = mb < 5 ? 5 : (mb > 100 ? 100 : mb);
        maxBytes = (long) v << 20;
    }

    static int maxMb() { return (int) (maxBytes >> 20); }

    static String path() { return file == null ? "-" : file.getAbsolutePath(); }

    static void i(String s) { write("I", s); }
    static void w(String s) { write("W", s); }
    static void e(String s) { write("E", s); }

    private static void write(String level, String msg) {
        if (!enabled || file == null) return;
        synchronized (L) {
            try {
                if (file.length() > maxBytes) rotate();
                String line = FMT.format(new Date()) + " [" + level + "] " + msg + "\n";
                FileOutputStream out = new FileOutputStream(file, true);
                out.write(line.getBytes(StandardCharsets.UTF_8));
                out.close();
            } catch (Throwable ignored) {
            }
        }
    }

    private static void rotate() {
        try {
            File old = new File(dir, "receiver.log.1");
            if (old.exists()) old.delete();
            file.renameTo(old);
        } catch (Throwable ignored) {
        }
    }
}
