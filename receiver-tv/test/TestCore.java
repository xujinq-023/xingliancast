package tv.projector.receiver;

/**
 * Host smoke test for the Android-free parts (Sps.parse, Annex-B csd extraction).
 * Compile+run on the PC JVM only; NOT packaged into the APK (kept under test\).
 *   javac -cp build\classes;. -d build\testout test\TestCore.java
 *   java  -cp build\classes;build\testout tv.projector.receiver.TestCore
 */
public final class TestCore {
    public static void main(String[] a) {
        int fails = 0;

        // ---- 1) baseline SPS vector (python bit-writer verified): 120x16 mbs,
        //         68 map units, fmo=1, crop bottom=4 (4:2:0) -> 1920x1080
        byte[] spsNal = hex("67420028ACF01E00897940");
        RxEngine.Cfg just = null;
        Sps s = Sps.parse(spsNal);
        if (s == null) { fails++; System.out.println("FAIL: Sps.parse returned null"); }
        else if (s.width != 1920 || s.height != 1080) {
            fails++;
            System.out.println("FAIL: Sps w/h = " + s.width + "x" + s.height + " expected 1920x1080");
        } else System.out.println("OK  : Sps 1920x1080 profile=" + s.profileIdc + " level=" + s.levelIdc);

        // ---- 2) Annex-B AU with SPS+PPS+IDR slice -> extractCfg must find both NALs
        byte[] ppsNal = hex("68CE4280");
        byte[] au = cat(hex("00000001"), spsNal,
                        hex("00000001"), ppsNal,
                        hex("00000001"), hex("65888400"),  // IDR slice, nal type 5
                        new byte[]{0, 0, 0, 0});
        RxEngine.Cfg c = RxEngine.extractCfg(au);
        if (c == null) { fails++; System.out.println("FAIL: extractCfg null"); }
        else {
            if (!java.util.Arrays.equals(c.sps, spsNal)) { fails++; System.out.println("FAIL: sps mismatch"); }
            if (!java.util.Arrays.equals(c.pps, ppsNal)) { fails++; System.out.println("FAIL: pps mismatch"); }
            if (fails == 0) System.out.println("OK  : extractCfg sps=" + c.sps.length + "B pps=" + c.pps.length + "B");
        }
        if (!RxEngine.hasNal(au, 5)) { fails++; System.out.println("FAIL: hasNal(5) false"); }
        else System.out.println("OK  : hasNal(type 5)");
        if (RxEngine.hasNal(au, 1)) { /* 0x65&0x1F==5, type 1 should be false */
            fails++; System.out.println("FAIL: hasNal(1) true for IDR-only AU"); }

        // ---- 3) 3-byte start code + emulated-prevention byte in SPS payload
        byte[] spsEmu = hex("6742001F96A2008000000300");  // contains 00 00 03
        byte[] au2 = cat(hex("000001"), spsEmu);
        RxEngine.Cfg c2 = RxEngine.extractCfg(au2);
        if (c2 == null || !java.util.Arrays.equals(c2.sps, spsEmu)) {
            fails++; System.out.println("FAIL: extractCfg with 3-byte start code");
        } else System.out.println("OK  : extractCfg 3-byte start code");

        System.out.println(fails == 0 ? "ALL PASS" : (fails + " FAILURES"));
        System.exit(fails == 0 ? 0 : 1);
    }

    private static byte[] hex(String h) {
        byte[] r = new byte[h.length() / 2];
        for (int i = 0; i < r.length; i++) {
            r[i] = (byte) Integer.parseInt(h.substring(i * 2, i * 2 + 2), 16);
        }
        return r;
    }

    private static byte[] cat(byte[]... parts) {
        int n = 0;
        for (byte[] p : parts) n += p.length;
        byte[] r = new byte[n];
        int o = 0;
        for (byte[] p : parts) { System.arraycopy(p, 0, r, o, p.length); o += p.length; }
        return r;
    }
}
