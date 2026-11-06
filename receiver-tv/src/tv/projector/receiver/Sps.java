package tv.projector.receiver;

/** Minimal H.264 SPS parser (exp-Golomb) for display width/height only. */
final class Sps {
    int width, height, profileIdc, levelIdc;

    /** @param nal full NAL unit including its 1-byte header (e.g. 0x67 ...) */
    static Sps parse(byte[] nal) {
        if (nal == null || nal.length < 4 || (nal[0] & 0x1F) != 7) return null;
        byte[] rbsp = deblock(nal, 1);
        Br br = new Br(rbsp);
        Sps s = new Sps();
        try {
            s.profileIdc = br.u(8);
            br.u(8);                                        // constraint flags + reserved
            s.levelIdc = br.u(8);
            br.ue();                                        // seq_parameter_set_id
            int chroma = 1;
            if (s.profileIdc == 66 || s.profileIdc == 77 || s.profileIdc == 88
                    || s.profileIdc == 100 || s.profileIdc == 110 || s.profileIdc == 122
                    || s.profileIdc == 244) {
                chroma = br.ue();                           // chroma_format_idc
                if (chroma == 3) br.u(1);                   // separate_colour_plane_flag
                br.ue();                                    // bit_depth_luma_minus8
                br.ue();                                    // bit_depth_chroma_minus8
                br.u(1);                                    // qpprime_y_zero_transform_bypass_flag
                if (br.u(1) != 0) {                         // seq_scaling_matrix_present_flag
                    int lists = (s.profileIdc != 66 && s.profileIdc != 77 && s.profileIdc != 88) ? 12 : 8;
                    for (int i = 0; i < lists; i++) {
                        if (br.u(1) != 0) scalingList(br, (i < 6) ? 16 : 64);
                    }
                }
            }
            br.ue();                                        // log2_max_frame_num_minus4
            int poc = br.ue();                              // pic_order_cnt_type
            if (poc == 0) br.ue();                          // log2_max_pic_order_cnt_lsb_minus4
            else if (poc == 1) {
                br.u(1); br.se(); br.se();
                int n = br.ue();
                for (int i = 0; i < n; i++) br.se();
            }
            br.ue();                                        // max_num_ref_frames
            br.u(1);                                        // gaps_in_frame_num_value_allowed_flag
            int picW = br.ue();                             // pic_width_in_mbs_minus1
            int picH = br.ue();                             // pic_height_in_map_units_minus1
            boolean fmo = br.u(1) != 0;                     // frame_mbs_only_flag
            if (!fmo) br.u(1);                              // mb_adaptive_frame_field_flag
            br.u(1);                                        // direct_8x8_inference_flag
            // CropUnitX/Y per H.264 table 7-11 (ChromaArrayType == chroma here)
            int cropX = (chroma == 1) ? 1 : 2;
            int cropY = ((chroma == 0) ? 1 : 2) * (fmo ? 1 : 2);
            if (br.u(1) != 0) {                             // frame_cropping_flag
                int l = br.ue(), r = br.ue(), t = br.ue(), b = br.ue();
                int w = (picW + 1) * 16 - (l + r) * cropX;
                int h = (picH + 1) * 16 * (fmo ? 1 : 2) - (t + b) * cropY;
                s.width = w; s.height = h;
                return s;
            }
            s.width = (picW + 1) * 16;
            s.height = (picH + 1) * 16 * (fmo ? 1 : 2);
            return s;
        } catch (RuntimeException e) {
            return null;
        }
    }

    private static void scalingList(Br br, int size) {
        int last = 8, next = 8;
        for (int i = 0; i < size; i++) {
            if (next != 16) {
                int delta = br.se();
                next = (last + delta + 256) & 0xFF;
            }
            if (next != 0) last = next;
        }
    }

    /** Remove emulation-prevention bytes (00 00 03 -> 00 00). */
    static byte[] deblock(byte[] src, int off) {
        byte[] out = new byte[src.length - off];
        int n = 0, zeros = 0;
        for (int i = off; i < src.length; i++) {
            byte b = src[i];
            if (zeros >= 2 && b == 3) { zeros = 0; continue; }
            zeros = (b == 0) ? zeros + 1 : 0;
            out[n++] = b;
        }
        if (n == out.length) return out;
        byte[] r = new byte[n];
        System.arraycopy(out, 0, r, 0, n);
        return r;
    }

    private static final class Br {
        private final byte[] d;
        private int pos;

        Br(byte[] d) { this.d = d; }

        int u(int n) {
            int v = 0;
            for (int i = 0; i < n; i++) v = (v << 1) | bit();
            return v;
        }

        int bit() {
            int i = pos >>> 3;
            if (i >= d.length) throw new IllegalStateException("sps overrun");
            int v = (d[i] >>> (7 - (pos & 7))) & 1;
            pos++;
            return v;
        }

        int ue() {
            int z = 0;
            while (bit() == 0) {
                z++;
                if (z > 31) throw new IllegalStateException("sps ue too long");
            }
            return (1 << z) - 1 + u(z);
        }

        int se() {
            int k = ue();
            return (k & 1) != 0 ? (k + 1) >>> 1 : -(k >>> 1);
        }
    }

    private Sps() { }
}
