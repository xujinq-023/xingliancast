// Projector sender UI (WinForms, custom-drawn dark theme).
// Layout: header (logo/title/version + settings/min/close) -> main card (settings + start/stop)
//         -> status bar (connection + live stats) -> footer (host/ip/network) -> optional log drawer.
// Wraps XingLianCastSender.exe: builds the command line, starts/stops the engine, shows its status.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.MemoryMappedFiles;
using System.Net;
using System.Net.NetworkInformation;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using Microsoft.Win32;
using System.Windows.Forms;

namespace ProjectorUi
{
    static class Program
    {
        [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr v);
        [DllImport("user32.dll")] static extern uint GetDpiForSystem();

        [STAThread]
        static void Main(string[] args)
        {
            // 诊断/支持用：不走界面，把结果写到文件，便于远程排查
            //   XingLianCast.exe --diag        机器码 / 授权状态 / 剩余试用
            //   XingLianCast.exe --diag <code> 校验指定激活码
            // 命令行注册：XingLianCast.exe --register "<激活码>"（支持远程指导用户自助注册）
            if (args != null && args.Length >= 2 && args[0].Equals("--register", StringComparison.OrdinalIgnoreCase))
            {
                string r = License.Register(args[1]);
                string msg = (r == null ? "OK " + License.StatusText() : "FAIL " + r);
                try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "xl_register.txt"), msg); } catch { }
                try { Console.WriteLine(msg); } catch { }
                return;
            }
            if (args != null && args.Length >= 1 && args[0].Equals("--license-ui", StringComparison.OrdinalIgnoreCase))
            {
                UIFont.Init();
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                using (var dlg = new LicenseForm()) dlg.ShowDialog();
                return;
            }
            if (args != null && args.Length >= 1 && args[0].Equals("--about", StringComparison.OrdinalIgnoreCase))
            {
                UIFont.Init();
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                using (var dlg = new AboutForm()) dlg.ShowDialog();
                return;
            }
            if (args != null && args.Length >= 1 && args[0].Equals("--diag", StringComparison.OrdinalIgnoreCase))
            {
                var sb = new StringBuilder();
                sb.AppendLine("machine = " + License.MachineCode());
                sb.AppendLine("status  = " + License.StatusText());
                sb.AppendLine("trial   = 剩余 " + Trial.RemainingText() + "（今日已用 " + Trial.UsedToday + " 秒）");
                if (args.Length >= 2)
                {
                    DateTime? exp; string note;
                    string err = License.Check(args[1], out exp, out note);
                    sb.AppendLine("check   = " + (err == null ? "OK" + (exp == null ? "（永久）" : "（至 " + exp.Value.ToLocalTime().ToString("yyyy-MM-dd") + "）") : err));
                }
                string outp = Path.Combine(Path.GetTempPath(), "xl_diag.txt");
                try { File.WriteAllText(outp, sb.ToString()); } catch { }
                try { Console.WriteLine(sb.ToString()); } catch { }
                return;
            }
            try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch { }
            AppDomain.CurrentDomain.UnhandledException += delegate(object s, UnhandledExceptionEventArgs e)
            {
                try { File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ui_crash.txt"),
                                         e.ExceptionObject.ToString()); } catch { }
            };
            try
            {
                UIFont.Init();
                Application.EnableVisualStyles();
                Application.SetCompatibleTextRenderingDefault(false);
                Application.Run(new MainForm());
            }
            catch (Exception ex)
            {
                try { File.WriteAllText(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "ui_crash.txt"), ex.ToString()); } catch { }
            }
        }
    }

    /// <summary>内嵌字体：Noto Sans SC 子集（SIL OFL 1.1），族名改为 XL UI，随程序分发。</summary>
    static class UIFont
    {
        public static string Family = "Microsoft YaHei UI";
        static IntPtr _data = IntPtr.Zero;

        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr LocalAlloc(uint flags, UIntPtr bytes);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr LocalFree(IntPtr h);

        public static void Init()
        {
            try
            {
                var asm = typeof(UIFont).Assembly;
                using (var s = asm.GetManifestResourceStream("XLUI.ttf"))
                {
                    if (s == null) return;
                    int n = (int)s.Length;
                    var buf = new byte[n];
                    int read = 0;
                    while (read < n) { int k = s.Read(buf, read, n - read); if (k <= 0) break; read += k; }
                    // AddMemoryFont 要求内存在字体集合生命周期内保持有效 → 非托管分配，不释放
                    _data = Marshal.AllocHGlobal(n);
                    Marshal.Copy(buf, 0, _data, n);
                    var pfc = new System.Drawing.Text.PrivateFontCollection();
                    pfc.AddMemoryFont(_data, n);
                    if (pfc.Families.Length > 0) Family = pfc.Families[0].Name;
                }
            }
            catch { Family = "Microsoft YaHei UI"; }
        }
    }

    // ---------------- theme ----------------
    static class T
    {
        public static float S = 1f;                       // dpi/96
        public static int I(float v) { return (int)Math.Round(v * S); }
        public static Size Sz(float w, float h) { return new Size(I(w), I(h)); }
        public static Point Pt(float x, float y) { return new Point(I(x), I(y)); }
        public static Padding Pad(float l, float t, float r, float b) { return new Padding(I(l), I(t), I(r), I(b)); }

        // 主题：Dark（默认）/ Light（Apple App Store 风格）
        public static bool Light = true;    // 默认浅色（App Store 风格）；PJS_UI_THEME=dark 可切换
        static Color C(Color dark, Color light) { return Light ? light : dark; }

        public static Color Bg1 { get { return C(Color.FromArgb(24, 18, 58), Color.FromArgb(242, 242, 247)); } }
        public static Color Bg2 { get { return C(Color.FromArgb(40, 26, 92), Color.FromArgb(242, 242, 247)); } }
        public static Color Card { get { return C(Color.FromArgb(36, 27, 78), Color.FromArgb(255, 255, 255)); } }
        public static Color Card2 { get { return C(Color.FromArgb(44, 33, 96), Color.FromArgb(242, 242, 247)); } }
        public static Color Border { get { return C(Color.FromArgb(66, 50, 128), Color.FromArgb(229, 229, 234)); } }
        public static Color Text { get { return C(Color.FromArgb(238, 234, 255), Color.FromArgb(28, 28, 30)); } }
        public static Color Sub { get { return C(Color.FromArgb(164, 158, 205), Color.FromArgb(142, 142, 147)); } }
        public static Color Accent { get { return C(Color.FromArgb(124, 92, 255), Color.FromArgb(64, 156, 255)); } }
        public static Color AccentHi { get { return C(Color.FromArgb(146, 116, 255), Color.FromArgb(95, 176, 255)); } }
        public static Color Cyan { get { return C(Color.FromArgb(45, 212, 238), Color.FromArgb(90, 200, 250)); } }
        public static Color Ok { get { return C(Color.FromArgb(52, 211, 153), Color.FromArgb(52, 199, 89)); } }
        public static Color Warn { get { return C(Color.FromArgb(251, 191, 36), Color.FromArgb(255, 149, 0)); } }
        public static Color Err { get { return C(Color.FromArgb(248, 113, 113), Color.FromArgb(255, 59, 48)); } }

        // 表面色
        public static Color HeaderBg { get { return C(Color.FromArgb(28, 21, 66), Color.FromArgb(255, 255, 255)); } }
        public static Color StatusBg { get { return C(Color.FromArgb(30, 22, 70), Color.FromArgb(255, 255, 255)); } }
        public static Color FooterBg { get { return C(Color.FromArgb(26, 19, 62), Color.FromArgb(242, 242, 247)); } }
        public static Color SettingsBg { get { return C(Color.FromArgb(32, 24, 74), Color.FromArgb(250, 250, 252)); } }
        public static Color Hairline { get { return C(Color.FromArgb(48, 36, 100), Color.FromArgb(229, 229, 234)); } }
        public static Color Hover { get { return C(Color.FromArgb(52, 40, 104), Color.FromArgb(242, 242, 247)); } }
        public static Color LogBg { get { return C(Color.FromArgb(22, 16, 52), Color.FromArgb(250, 250, 252)); } }
        public static Color LogText { get { return C(Color.FromArgb(190, 185, 225), Color.FromArgb(60, 60, 67)); } }
        public static Color Muted { get { return C(Color.FromArgb(120, 114, 165), Color.FromArgb(142, 142, 147)); } }
        public static Color FooterText { get { return C(Color.FromArgb(140, 134, 180), Color.FromArgb(142, 142, 147)); } }
        public static Color Idle { get { return C(Color.FromArgb(120, 116, 150), Color.FromArgb(174, 174, 178)); } }
        public static Color DisabledFill { get { return C(Color.FromArgb(46, 40, 78), Color.FromArgb(229, 229, 234)); } }
        public static Color DisabledFill2 { get { return C(Color.FromArgb(60, 52, 96), Color.FromArgb(242, 242, 247)); } }
        public static Color DisabledText { get { return C(Color.FromArgb(120, 116, 150), Color.FromArgb(174, 174, 178)); } }
        public static Color Glyph { get { return C(Color.FromArgb(198, 192, 232), Color.FromArgb(60, 60, 67)); } }
        public static Color StopFill { get { return C(Color.FromArgb(52, 40, 104), Color.FromArgb(242, 242, 247)); } }
        public static Color StopStroke { get { return C(Color.FromArgb(90, 72, 170), Color.FromArgb(209, 209, 214)); } }
        public static Color StopText { get { return C(Color.FromArgb(238, 234, 255), Color.FromArgb(64, 156, 255)); } }
        public static Color SelectBg { get { return C(Color.FromArgb(58, 44, 118), Color.FromArgb(224, 238, 255)); } }   // 下拉选中底色（浅蓝）
        public static Color SelectFg { get { return C(Color.FromArgb(238, 234, 255), Color.FromArgb(20, 20, 22)); } }

        public static Font Title(float px)
        {
            string fam = UIFont.Family;
            try { using (var ff = new FontFamily(fam)) { } }
            catch { fam = "Microsoft YaHei UI"; }
            return new Font(fam, px * S, FontStyle.Regular, GraphicsUnit.Pixel);   // 字重已烘进字体(600)
        }
        public static Font Font(string family, float px, bool bold)
        {
            return new Font(family, px * S, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        }
        public static Font F(float px, bool bold = false)
        {
            return new Font("Microsoft YaHei UI", px * S, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
        }
        public static void Rounded(Graphics g, Rectangle r, int radius, Color fill, Color? border = null)
        {
            using (var p = Round(r, radius))
            using (var b = new SolidBrush(fill))
            {
                g.FillPath(b, p);
                if (border.HasValue)
                    using (var pen = new Pen(border.Value)) g.DrawPath(pen, p);
            }
        }
        public static GraphicsPath Round(Rectangle r, int radius)
        {
            int d = radius * 2;
            var p = new GraphicsPath();
            p.AddArc(r.X, r.Y, d, d, 180, 90);
            p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            p.CloseFigure();
            return p;
        }
    }

    // ---------------- custom controls ----------------
    class BufPanel : Panel
    {
        public BufPanel() { DoubleBuffered = true; ResizeRedraw = true; }
        protected override void OnPaintBackground(PaintEventArgs e)
        {
            if (BackColor.A == 255) e.Graphics.Clear(BackColor);
            else base.OnPaintBackground(e);
        }
    }

    class GradientPanel : Panel
    {
        public GradientPanel() { DoubleBuffered = true; ResizeRedraw = true; }
        protected override void OnPaintBackground(PaintEventArgs e)
        {
            using (var b = new LinearGradientBrush(ClientRectangle, T.Bg1, T.Bg2, 35f))
                e.Graphics.FillRectangle(b, ClientRectangle);
        }
    }

    class Card : Panel
    {
        public int Radius = 12;
        public Card() { DoubleBuffered = true; BackColor = Color.Transparent; }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            if (T.Light)
            {
                for (int i = 3; i >= 1; i--)
                {
                    var sr = new Rectangle(r.X + 1, r.Y + i + 1, r.Width - 2, r.Height - 2);
                    using (var p0 = T.Round(sr, Radius))
                    using (var b0 = new SolidBrush(Color.FromArgb(6 + (3 - i) * 4, 0, 0, 0)))
                        g.FillPath(b0, p0);
                }
            }
            T.Rounded(g, r, Radius, T.Card, T.Border);
        }
    }

    class FlatButton : Button
    {
        public Color Fill = T.Card2;
        public Color FillHover = T.Card2;
        public Color TextColor = T.Text;
        public Color Stroke = Color.Transparent;
        public int Radius = 10;
        bool _hover, _down;
        public FlatButton()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint | ControlStyles.OptimizedDoubleBuffer, true);
            FlatStyle = FlatStyle.Flat;
            FlatAppearance.BorderSize = 0;
            Font = T.F(13);
            BackColor = T.Card;
            Cursor = Cursors.Hand;
        }
        protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hover = false; _down = false; Invalidate(); base.OnMouseLeave(e); }
        protected override void OnMouseDown(MouseEventArgs e) { _down = true; Invalidate(); base.OnMouseDown(e); }
        protected override void OnMouseUp(MouseEventArgs e) { _down = false; Invalidate(); base.OnMouseUp(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var bg = new SolidBrush(BackColor)) g.FillRectangle(bg, ClientRectangle);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            Color fill = !Enabled ? T.DisabledFill
                        : (_down ? Color.FromArgb(Math.Max(FillHover.R - 18, 0), Math.Max(FillHover.G - 18, 0), Math.Max(FillHover.B - 18, 0))
                                 : (_hover ? FillHover : Fill));
            T.Rounded(g, r, Radius, fill, Stroke == Color.Transparent ? (Color?)null : Stroke);
            TextRenderer.DrawText(g, Text, Font, r, Enabled ? TextColor : T.DisabledText,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }

    class AccentButton : FlatButton
    {
        public Color Base = T.Accent, Base2 = T.AccentHi;   // 运行时可换（绿/橙/蓝）
        public AccentButton()
        {
            Fill = T.Accent; FillHover = T.AccentHi; TextColor = Color.White; Radius = 10; Font = T.F(14, true);
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            using (var bg = new SolidBrush(BackColor)) g.FillRectangle(bg, ClientRectangle);
            g.SmoothingMode = SmoothingMode.AntiAlias;
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            using (var b = new LinearGradientBrush(r, Enabled ? Base : T.DisabledFill2,
                                                      Enabled ? Base2 : T.DisabledFill, 20f))
            using (var p = T.Round(r, Radius))
            {
                g.FillPath(b, p);
            }
            TextRenderer.DrawText(g, Text, Font, r, Enabled ? Color.White : T.DisabledText,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
        }
    }

    enum Glyph { Menu, Min, Close }

    class GlyphButton : FlatButton
    {
        public Glyph Kind = Glyph.Menu;
        bool _hovered;
        public GlyphButton() { Fill = T.HeaderBg; FillHover = T.Hover; BackColor = T.HeaderBg; Text = ""; Radius = 8; }
        protected override void OnMouseEnter(EventArgs e) { _hovered = true; base.OnMouseEnter(e); }
        protected override void OnMouseLeave(EventArgs e) { _hovered = false; base.OnMouseLeave(e); }
        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var pen = new Pen(_hovered ? T.Accent : T.Glyph, T.I(1.7f)))
            {
                pen.StartCap = LineCap.Round;
                pen.EndCap = LineCap.Round;
                float cx = Width / 2f, cy = Height / 2f;
                float half = T.I(15) / 2f;       // 横线半宽
                float gap = T.I(4.2f);           // 三行严格等距
                if (Kind == Glyph.Close)
                {
                    float d = T.I(5.4f);
                    g.DrawLine(pen, cx - d, cy - d, cx + d, cy + d);
                    g.DrawLine(pen, cx - d, cy + d, cx + d, cy - d);
                }
                else if (Kind == Glyph.Min)
                {
                    g.DrawLine(pen, cx - half, cy, cx + half, cy);
                }
                else
                {
                    g.DrawLine(pen, cx - half, cy - gap, cx + half, cy - gap);
                    g.DrawLine(pen, cx - half, cy, cx + half, cy);
                    g.DrawLine(pen, cx - half, cy + gap, cx + half, cy + gap);
                }
            }
        }
    }

    /// <summary>标题：圆润字体 + 渐变填充 + 轻投影。</summary>
    class TitleBox : Control
    {
        public TitleBox()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.UserPaint | ControlStyles.SupportsTransparentBackColor, true);
            BackColor = Color.Transparent;
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.AntiAlias;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var f = T.Title(25))
            {
                var fmt = new StringFormat(StringFormat.GenericTypographic);
                fmt.FormatFlags |= StringFormatFlags.NoWrap;
                fmt.LineAlignment = StringAlignment.Center;
                fmt.Alignment = StringAlignment.Near;
                var rect = new RectangleF(0, T.I(-2), Width, Height);
                if (T.Light)
                    using (var shadow = new SolidBrush(Color.FromArgb(38, 60, 130, 220)))
                    {
                        var r2 = rect; r2.Offset(1.2f, 1.6f);
                        g.DrawString(Text, f, shadow, r2, fmt);
                    }
                using (var br = new LinearGradientBrush(
                           new Rectangle(0, 0, Math.Max(1, Width), Math.Max(1, Height)),
                           T.Light ? Color.FromArgb(58, 120, 246) : Color.FromArgb(150, 120, 255),
                           T.Light ? Color.FromArgb(0, 176, 214) : Color.FromArgb(45, 212, 238), 0f))
                    g.DrawString(Text, f, br, rect, fmt);
            }
        }
    }

    /// <summary>指标卡片：圆角浅底 + 小标题 + 大号数值。</summary>
    class MetricCard : Control
    {
        public string Caption = "";
        string _value = "—";
        public MetricCard()
        {
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint, true);
            BackColor = T.Card;
        }
        public void Set(string v) { if (v != _value) { _value = v; Invalidate(); } }
        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            using (var bg = new SolidBrush(BackColor)) g.FillRectangle(bg, ClientRectangle);
            var r = new Rectangle(0, 0, Width - 1, Height - 1);
            T.Rounded(g, r, T.I(10), T.Card2, T.Border);
            int px = T.I(11), pt = T.I(8);
            TextRenderer.DrawText(g, Caption, T.F(11),
                new Rectangle(px, pt, Width - px * 2, T.I(16)), T.Sub,
                TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.EndEllipsis);
            TextRenderer.DrawText(g, _value, T.F(12),         // 与标题同族同色同号、不加粗
                new Rectangle(px, pt + T.I(18), Width - px * 2, T.I(24)), T.Sub,
                TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.EndEllipsis);
        }
    }

    /// <summary>
    /// 授权：ECDSA P-256 离线验签。客户端只内置公钥，私钥仅在发码工具里，逆向也伪造不出有效码。
    /// 码格式：XL1.&lt;base64url(payload 12B + signature 64B)&gt;
    /// payload = ver(1) + machineHash(6) + expiryDays(4, BE) + flags(1)
    /// </summary>
    /// <summary>
    /// 授权/试用数据的存储：某些机器会拒绝写 %LOCALAPPDATA%（杀软、受控文件夹访问、企业策略），
    /// 所以按 用户目录 → Roaming → 程序目录 → TEMP → 注册表 依次回退。
    /// </summary>
    static class Store
    {
        static string[] DirCandidates()
        {
            var list = new List<string>();
            try { list.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "星联投屏")); } catch { }
            try { list.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "星联投屏")); } catch { }
            try { list.Add(Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "data")); } catch { }
            try { list.Add(Path.Combine(Path.GetTempPath(), "星联投屏")); } catch { }
            return list.ToArray();
        }

        static string RegKey { get { return @"Software\XingLian\Projector"; } }

        /// <summary>读取：先找第一个存在的文件，其次读注册表。</summary>
        public static string Read(string name)
        {
            foreach (var d in DirCandidates())
            {
                try
                {
                    string p = Path.Combine(d, name);
                    if (File.Exists(p))
                    {
                        string s = File.ReadAllText(p).Trim();
                        if (s.Length > 0) return s;
                    }
                }
                catch { }
            }
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(RegKey))
                    if (k != null) return (k.GetValue(name) as string ?? "").Trim();
            }
            catch { }
            return "";
        }

        /// <summary>写入：任一位置成功即可，同时尽量写注册表做备份。返回成功的路径（失败返回 null）。</summary>
        public static string Write(string name, string content)
        {
            string okPath = null;
            // 写所有能写的位置做冗余（旧版第一个成功就 break，别处会留旧副本；
            // 且只有注册表成功时返回 null → 误报"保存失败"）
            foreach (var d in DirCandidates())
            {
                try
                {
                    Directory.CreateDirectory(d);
                    string p = Path.Combine(d, name);
                    File.WriteAllText(p, content);
                    // 读回来确认真的写进去了（有些环境 WriteAllText 静默失败）
                    if (File.Exists(p) && File.ReadAllText(p).Trim() == content.Trim())
                        if (okPath == null) okPath = p;
                }
                catch { }
            }
            bool regOk = false;
            try
            {
                using (var k = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(RegKey))
                    if (k != null) { k.SetValue(name, content); regOk = true; }
            }
            catch { }
            if (okPath == null && regOk) okPath = @"HKCU\" + RegKey;   // 只有注册表成功也算成功
            return okPath;
        }
    }


    static class License
    {
        const string PubX = "2Z7kYMRV9Tt3n638nxJbIL5RZ54GRAIo2GkduUqAxK8=";
        const string PubY = "j5/5Bkmi/0B6g6n8N2PMwD6psGXzXxZiweDxDZztg+Y=";
        public const int PayloadLen = 12;
        public const uint NeverExpire = 0xFFFFFFFFu;
        static readonly DateTime Epoch = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);

        /// <summary>机器码：机器名 + 系统盘卷信息 + CPU 核数，SHA256 前 12 位十六进制。</summary>
        public static string MachineCode()
        {
            try
            {
                string vol = "0";
                try
                {
                    var d = new DriveInfo(Path.GetPathRoot(Environment.SystemDirectory));
                    if (d != null) vol = d.IsReady ? d.VolumeLabel + ":" + d.TotalSize : "0";
                }
                catch { }
                string raw = Environment.MachineName + "|" + vol + "|" + Environment.ProcessorCount;
                using (var sha = SHA256.Create())
                {
                    byte[] h = sha.ComputeHash(Encoding.UTF8.GetBytes(raw));
                    var sb = new StringBuilder();
                    for (int i = 0; i < 6; i++) sb.Append(h[i].ToString("X2"));
                    string hex = sb.ToString();
                    return hex.Substring(0, 4) + "-" + hex.Substring(4, 4) + "-" + hex.Substring(8, 4);
                }
            }
            catch { return "0000-0000-0000"; }
        }

        /// <summary>机器码的 6 字节摘要（写进授权载荷）。</summary>
        public static byte[] MachineHash()
        {
            using (var sha = SHA256.Create())
                return sha.ComputeHash(Encoding.UTF8.GetBytes(MachineCode()));
        }

        static ECDsa PublicKey()
        {
            var p = new ECParameters();
            p.Curve = ECCurve.NamedCurves.nistP256;
            p.Q = new ECPoint { X = Convert.FromBase64String(PubX), Y = Convert.FromBase64String(PubY) };
            var ec = ECDsa.Create();
            ec.ImportParameters(p);
            return ec;
        }

        public static string Base64Url(byte[] b)
        {
            return Convert.ToBase64String(b).Replace('+', '-').Replace('/', '_').TrimEnd('=');
        }

        public static byte[] FromBase64Url(string s)
        {
            s = s.Replace('-', '+').Replace('_', '/');
            switch (s.Length % 4) { case 2: s += "=="; break; case 3: s += "="; break; }
            return Convert.FromBase64String(s);
        }

        /// <summary>校验授权码。返回 null 表示通过，否则返回失败原因。</summary>
        public static string Check(string code, out DateTime? expiry, out string flagsNote)
        {
            expiry = null; flagsNote = "";
            if (string.IsNullOrWhiteSpace(code)) return "没有输入激活码";
            code = code.Trim();
            if (!code.StartsWith("XL1.", StringComparison.OrdinalIgnoreCase)) return "激活码格式不正确（应以 XL1. 开头）";
            byte[] all;
            try { all = FromBase64Url(code.Substring(4)); }
            catch { return "激活码内容损坏"; }
            if (all.Length != PayloadLen + 64) return "激活码长度不正确";

            var payload = new byte[PayloadLen];
            var sig = new byte[64];
            Array.Copy(all, 0, payload, 0, PayloadLen);
            Array.Copy(all, PayloadLen, sig, 0, 64);

            bool ok;
            try
            {
                using (var ec = PublicKey())
                using (var sha = SHA256.Create())
                    ok = ec.VerifyHash(sha.ComputeHash(payload), sig);
            }
            catch { return "验签失败（公钥异常）"; }
            if (!ok) return "激活码无效（签名校验失败）";

            if (payload[0] != 1) return "激活码版本不支持";
            byte[] mine = MachineHash();
            for (int i = 0; i < 6; i++)
                if (payload[1 + i] != mine[i]) return "激活码与本机机器码不匹配（换机器了？）";

            uint days = ((uint)payload[7] << 24) | ((uint)payload[8] << 16) | ((uint)payload[9] << 8) | payload[10];
            if (days != NeverExpire)
            {
                var exp = Epoch.AddDays(days);
                expiry = exp;
                if (DateTime.UtcNow.Date > exp.Date) return "激活码已过期（" + exp.ToLocalTime().ToString("yyyy-MM-dd") + "）";
            }
            if ((payload[11] & 1) == 0) return "该激活码未授予投屏功能";
            flagsNote = "永久".Equals("") ? "" : "";
            return null;   // 通过
        }


        public static string SavedKey() { return Store.Read("license.dat"); }

        public static DateTime? Expiry;
        public static bool IsRegistered
        {
            get
            {
                string k = SavedKey();
                if (k.Length == 0) return false;
                DateTime? exp; string note;
                return Check(k, out exp, out note) == null;
            }
        }

        /// <summary>保存授权码（内部先校验）。</summary>
        public static string Register(string code)
        {
            DateTime? exp; string note;
            string err = Check(code, out exp, out note);
            if (err != null) return err;
            if (Store.Write("license.dat", code.Trim()) == null)
                return "保存失败：所有存储位置都不可写（可能被安全软件限制）";
            Expiry = exp;
            return null;
        }

        public static string StatusText()
        {
            string k = SavedKey();
            if (k.Length == 0) return "未注册";
            DateTime? exp; string note;
            string err = Check(k, out exp, out note);
            if (err != null) return "未注册（" + err + "）";
            return exp == null ? "已注册（永久授权）" : "已注册（有效期至 " + exp.Value.ToLocalTime().ToString("yyyy-MM-dd") + "）";
        }
    }

    /// <summary>未注册用户：每天可试用 5 分钟（累计，按自然日重置）。</summary>
    static class Trial
    {
        public const int LimitSecondsPerDay = 300;
        const string RegPath = @"Software\XingLian\Projector";

        static string StorePath
        {
            get
            {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "星联投屏");
                Directory.CreateDirectory(dir);
                return Path.Combine(dir, "trial.dat");
            }
        }

        /// <summary>返回 (日期, 已用秒数)：取文件与注册表中的较大值，简单清理一处不生效。</summary>
        static string _memDate = "";
        static int _memUsed;

        static void ReadRaw(out string date, out int used)
        {
            date = ""; used = 0;
            string f = null, r = null;
            try { f = Store.Read("trial.dat"); } catch { }
            string fe = "";
            try { fe = Store.Read("engine_trial.dat"); } catch { }   // 引擎自己记的额度（更权威）
            try
            {
                using (var k = Registry.CurrentUser.OpenSubKey(RegPath))
                    if (k != null) r = (k.GetValue("Trial") as string ?? "").Trim();
            }
            catch { }
            ParseBetter(f, ref date, ref used);
            ParseBetter(fe, ref date, ref used);
            ParseBetter(r, ref date, ref used);
            // 内存兜底：文件/注册表都写不进去时（受限环境），至少本次运行内限制仍然有效
            if (_memDate == date) { if (_memUsed > used) used = _memUsed; }
            else if (!string.IsNullOrEmpty(_memDate) && string.CompareOrdinal(_memDate, date) > 0) { date = _memDate; used = _memUsed; }
        }

        static void ParseBetter(string s, ref string date, ref int used)
        {
            if (string.IsNullOrEmpty(s)) return;
            var parts = s.Split('|');
            if (parts.Length != 2) return;
            int u;
            if (!int.TryParse(parts[1], out u)) return;
            if (s.Length == 0) return;
            // 同一天取较大值；不同天直接用新日期
            if (parts[0] == date) { if (u > used) used = u; }
            else if (string.CompareOrdinal(parts[0], date) > 0) { date = parts[0]; used = u; }
        }

        static void Write(string date, int used)
        {
            _memDate = date; _memUsed = used;
            string s = date + "|" + used;
            try { Store.Write("trial.dat", s); } catch { }
            try
            {
                using (var k = Registry.CurrentUser.CreateSubKey(RegPath))
                    if (k != null) k.SetValue("Trial", s);
            }
            catch { }
        }

        public static int UsedToday
        {
            get
            {
                string d; int u;
                ReadRaw(out d, out u);
                return d == Today ? Math.Min(u, LimitSecondsPerDay) : 0;
            }
        }

        public static int RemainingToday
        {
            get { return Math.Max(0, LimitSecondsPerDay - UsedToday); }
        }

        static string Today { get { return DateTime.Now.ToString("yyyy-MM-dd"); } }

        /// <summary>累加已用秒数（跨天自动归零）。</summary>
        public static void Add(int seconds)
        {
            if (seconds <= 0) return;
            string d; int u;
            ReadRaw(out d, out u);
            if (d != Today) { d = Today; u = 0; }
            u += seconds;
            if (u > LimitSecondsPerDay) u = LimitSecondsPerDay;
            Write(d, u);
        }

        /// <summary>剩余时间的可读形式：mm:ss</summary>
        public static string RemainingText()
        {
            int r = RemainingToday;
            return (r / 60) + ":" + (r % 60).ToString("00");
        }
    }

    class LicenseForm : Form
    {
        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);

        void DragMove(Control ctl)
        {
            if (ctl == null) return;
            ctl.MouseDown += delegate(object s, MouseEventArgs e)
            {
                if (e.Button != MouseButtons.Left) return;
                ReleaseCapture();
                SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);
            };
        }

        readonly TextBox _key;
        readonly Label2 _state;

        public LicenseForm()
        {
            Text = "正版注册";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = T.Sz(440, 392);
            BackColor = T.Card;
            AutoScaleMode = AutoScaleMode.None;
            ShowInTaskbar = false;
            SetStyle(ControlStyles.ResizeRedraw | ControlStyles.AllPaintingInWmPaint |
                     ControlStyles.OptimizedDoubleBuffer, true);

            // 圆角边框 + 标题分隔线（无边框窗口需要自己画边界）
            Paint += delegate(object s, PaintEventArgs e)
            {
                var g = e.Graphics;
                g.SmoothingMode = SmoothingMode.AntiAlias;
                using (var path = new GraphicsPath())
                {
                    int r = T.I(10), d = r * 2;
                    var rc = new Rectangle(0, 0, Width - 1, Height - 1);
                    path.AddArc(rc.X, rc.Y, d, d, 180, 90);
                    path.AddArc(rc.Right - d - 1, rc.Y, d, d, 270, 90);
                    path.AddArc(rc.Right - d - 1, rc.Bottom - d - 1, d, d, 0, 90);
                    path.AddArc(rc.X, rc.Bottom - d - 1, d, d, 90, 90);
                    path.CloseFigure();
                    using (var pen = new Pen(Color.FromArgb(0xC5, 0xC5, 0xD0))) g.DrawPath(pen, path);
                }
                using (var pen = new Pen(T.Hairline))
                    g.DrawLine(pen, T.I(18), T.I(60), Width - T.I(18), T.I(60));
            };

            var title = new TitleBox();
            title.Text = "正版注册";
            title.Location = T.Pt(22, 12);
            title.Size = T.Sz(260, 42);
            Controls.Add(title);
            DragMove(title);

            bool ok = License.IsRegistered;
            _state = new Label2();
            _state.Font = T.F(12);
            _state.ForeColor = ok ? Color.FromArgb(0x2E, 0xA5, 0x62) : Color.FromArgb(0xE0, 0x7A, 0x3C);
            _state.Location = T.Pt(24, 70);
            _state.Size = T.Sz(392, 22);
            _state.Text = ok ? License.StatusText() : "状态：未注册 · 今日试用剩余 " + Trial.RemainingText();
            Controls.Add(_state);

            var mcCap = new Label2();
            mcCap.Text = "本机机器码（复制给发行方换取激活码）";
            mcCap.Font = T.F(11);
            mcCap.ForeColor = T.Sub;
            mcCap.Location = T.Pt(24, 104);
            mcCap.Size = T.Sz(392, 20);
            Controls.Add(mcCap);

            var mc = new TextBox();
            mc.Text = License.MachineCode();
            mc.Font = T.F(13);
            mc.ReadOnly = true;
            mc.BorderStyle = BorderStyle.FixedSingle;
            mc.Location = T.Pt(24, 126);
            mc.Size = T.Sz(292, 28);
            Controls.Add(mc);

            var btnCopy = new FlatButton();
            btnCopy.Text = "复制";
            btnCopy.Location = T.Pt(324, 124);
            btnCopy.Size = T.Sz(92, 32);
            btnCopy.Fill = T.Card2; btnCopy.FillHover = T.Border; btnCopy.TextColor = T.Sub;
            btnCopy.Click += delegate
            {
                try { Clipboard.SetText(mc.Text); btnCopy.Text = "已复制"; }
                catch { }
            };
            Controls.Add(btnCopy);

            var keyCap = new Label2();
            keyCap.Text = "激活码（整段粘贴，约 106 个字符）";
            keyCap.Font = T.F(11);
            keyCap.ForeColor = T.Sub;
            keyCap.Location = T.Pt(24, 168);
            keyCap.Size = T.Sz(392, 20);
            Controls.Add(keyCap);

            _key = new TextBox();
            _key.Text = License.SavedKey();
            _key.Font = T.F(10.5f);
            _key.BorderStyle = BorderStyle.FixedSingle;
            _key.Multiline = true;
            _key.ScrollBars = ScrollBars.Vertical;
            _key.WordWrap = true;
            _key.Location = T.Pt(24, 190);
            _key.Size = T.Sz(392, 76);
            Controls.Add(_key);

            const string BuyText = "购买授权：xujinq@hotmail.com　·　+86 13996351705\n" +
                                   "10 元/年　·　58 元终生　　点击此处复制联系方式";
            const string ContactText = "xujinq@hotmail.com  /  +86 13996351705";

            var buy = new Label2();
            buy.Text = BuyText;
            buy.Font = T.F(11);
            buy.ForeColor = T.Sub;
            buy.Location = T.Pt(24, 274);
            buy.Size = T.Sz(392, 46);
            buy.Cursor = Cursors.Hand;
            buy.Click += delegate
            {
                // 复制内容始终是联系方式本身（不是标签文字），文本只做临时提示，1.5 秒后还原
                try { Clipboard.SetText(ContactText); } catch { }
                buy.Text = "✓ 联系方式已复制到剪贴板";
                buy.ForeColor = Color.FromArgb(0x2E, 0xA5, 0x62);
                var t = new System.Windows.Forms.Timer();
                t.Interval = 1500;
                t.Tick += delegate
                {
                    t.Stop(); t.Dispose();
                    buy.Text = BuyText;
                    buy.ForeColor = T.Sub;
                };
                t.Start();
            };
            Controls.Add(buy);
            var btnOk = new AccentButton();
            btnOk.Text = "注册";
            btnOk.Location = T.Pt(300, 336);
            btnOk.Size = T.Sz(116, 34);
            btnOk.Click += delegate
            {
                string err = License.Register(_key.Text);
                if (err == null)
                {
                    _state.Text = License.StatusText();
                    _state.ForeColor = Color.FromArgb(0x2E, 0xA5, 0x62);
                    MessageBox.Show(this, "注册成功，感谢支持！", "星联投屏",
                        MessageBoxButtons.OK, MessageBoxIcon.Information);
                    DialogResult = DialogResult.OK;
                    Close();
                }
                else
                {
                    _state.Text = err;
                    _state.ForeColor = T.Err;
                }
            };
            Controls.Add(btnOk);

            var btnClose = new FlatButton();
            btnClose.Text = "关闭";
            btnClose.Location = T.Pt(24, 336);
            btnClose.Size = T.Sz(96, 34);
            btnClose.Fill = T.Card2; btnClose.FillHover = T.Border; btnClose.TextColor = T.Sub;
            btnClose.Click += delegate { Close(); };
            Controls.Add(btnClose);

            // 未注册时每秒刷新剩余试用时间
            if (!ok)
            {
                var tick = new System.Windows.Forms.Timer();
                tick.Interval = 1000;
                tick.Tick += delegate
                {
                    if (License.IsRegistered) { tick.Stop(); _state.Text = License.StatusText(); return; }
                    _state.Text = "状态：未注册 · 今日试用剩余 " + Trial.RemainingText();
                };
                tick.Start();
                FormClosed += delegate { tick.Stop(); tick.Dispose(); };
            }

            DragMove(this);
        }
    }

    class Label2 : Label
    {
        public Label2() { BackColor = Color.Transparent; ForeColor = T.Sub; Font = T.F(13); AutoSize = false; TextAlign = ContentAlignment.MiddleLeft; }
    }

    // ---------------- monitor enumeration (order must match sender: primary first) ----------------
    class MonitorInfo
    {
        public string Device;
        public string Model;
        public int Width, Height;
        public bool Primary;
        public override string ToString()
        {
            string name = string.IsNullOrEmpty(Model) ? CleanName(Device) : Model;
            return name + "  ·  " + Width + "x" + Height + (Primary ? "  (主屏)" : "");
        }
        public static string CleanName(string dev)
        {
            if (string.IsNullOrEmpty(dev)) return "显示器";
            return dev.StartsWith(@"\\.\") ? dev.Substring(4) : dev;
        }
    }

    static class Monitors
    {
        delegate bool EnumProc(IntPtr hMon, IntPtr hdc, IntPtr rect, IntPtr data);

        [StructLayout(LayoutKind.Sequential)]
        struct RECT { public int Left, Top, Right, Bottom; }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct MONITORINFOEX
        {
            public int cbSize;
            public RECT rcMonitor;
            public RECT rcWork;
            public uint dwFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string szDevice;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct DISPLAY_DEVICE
        {
            public int cb;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 32)] public string DeviceName;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceString;
            public int StateFlags;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceID;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string DeviceKey;
        }

        [DllImport("user32.dll")]
        static extern bool EnumDisplayMonitors(IntPtr hdc, IntPtr clip, EnumProc proc, IntPtr data);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool GetMonitorInfoW(IntPtr hMon, ref MONITORINFOEX mi);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        static extern bool EnumDisplayDevicesW(string device, uint devNum, ref DISPLAY_DEVICE dd, uint flags);

        static string MonitorModel(string gdiName)
        {
            try
            {
                var dd = new DISPLAY_DEVICE();
                dd.cb = Marshal.SizeOf(typeof(DISPLAY_DEVICE));
                if (!EnumDisplayDevicesW(gdiName, 0, ref dd, 0)) return null;
                if (string.IsNullOrEmpty(dd.DeviceID)) return null;
                // DeviceID: MONITOR\LEN61B5\{...}\0003 -- 注册表实例名是 5&..&UID.. 形式，按硬件 ID 枚举
                string[] p = dd.DeviceID.Split('\\');
                if (p.Length < 2) return null;
                using (var hw = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(
                           @"SYSTEM\CurrentControlSet\Enum\DISPLAY\" + p[1]))
                {
                    if (hw == null) return null;
                    foreach (string inst in hw.GetSubKeyNames())
                        using (var dp = hw.OpenSubKey(inst + @"\Device Parameters"))
                        {
                            if (dp == null) continue;
                            string name = EdidName(dp.GetValue("EDID") as byte[]);
                            if (!string.IsNullOrEmpty(name)) return name;
                        }
                }
            }
            catch { }
            return null;
        }

        static string EdidName(byte[] edid)
        {
            if (edid == null || edid.Length < 54 + 4 * 18) return null;
            for (int b = 0; b < 4; b++)
            {
                int off = 54 + b * 18;
                if (edid[off] != 0 || edid[off + 1] != 0 || edid[off + 2] != 0) continue;
                byte tag = edid[off + 3];
                if (tag != 0xFC && tag != 0xFE) continue;
                var sb = new StringBuilder(14);
                for (int i = 5; i < 18; i++)
                {
                    byte ch = edid[off + i];
                    if (ch == 0x0A || ch == 0x00) break;
                    sb.Append((char)ch);
                }
                string s = sb.ToString().Trim();
                if (s.Length > 0) return s;
            }
            return null;
        }

        public static List<MonitorInfo> List()
        {
            var list = new List<MonitorInfo>();
            EnumDisplayMonitors(IntPtr.Zero, IntPtr.Zero, delegate(IntPtr h, IntPtr hdc, IntPtr r, IntPtr d)
            {
                var mi = new MONITORINFOEX();
                mi.cbSize = Marshal.SizeOf(typeof(MONITORINFOEX));
                if (GetMonitorInfoW(h, ref mi))
                    list.Add(new MonitorInfo
                    {
                        Device = mi.szDevice,
                        Model = MonitorModel(mi.szDevice),
                        Width = mi.rcMonitor.Right - mi.rcMonitor.Left,
                        Height = mi.rcMonitor.Bottom - mi.rcMonitor.Top,
                        Primary = (mi.dwFlags & 1) != 0
                    });
                return true;
            }, IntPtr.Zero);
            list.Sort(delegate(MonitorInfo a, MonitorInfo b)
            {
                if (a.Primary && !b.Primary) return -1;
                if (!a.Primary && b.Primary) return 1;
                return 0;
            });
            return list;
        }
    }

    // ---------------- LAN discovery (UDP 45691 beacon from the TV) ----------------
    class Discovery
    {
        public const int Port = 45691;
        public delegate void FoundHandler(List<Device> devices);
        public class Device
        {
            public string Name = "TV", Ip = "", Model = "";
            public int Port = 45690;
            public override string ToString() { return Name + "  ·  " + Ip; }
        }

        readonly Dictionary<string, Device> _devices = new Dictionary<string, Device>();
        Thread _thread;
        volatile bool _run;
        UdpClient _udp;

        // 句柄继承：界面用 RedirectStandardOutput 启动 sender 时会继承可继承句柄，
        // 发现 socket 被继承后，界面退出仍会被孤儿子进程占用端口 → 新界面永远搜不到设备。
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
        const uint HANDLE_FLAG_INHERIT = 0x1;

        public void Start(FoundHandler onFound)
        {
            _run = true;
            _thread = new Thread(delegate()
            {
                while (_run)
                {
                    try
                    {
                        _udp = new UdpClient(Port);
                        _udp.Client.ReceiveTimeout = 0;
                        try { SetHandleInformation(_udp.Client.Handle, HANDLE_FLAG_INHERIT, 0); } catch { }
                        var ep = new IPEndPoint(IPAddress.Any, 0);
                        while (_run)
                        {
                            byte[] data = _udp.Receive(ref ep);
                            string text = Encoding.UTF8.GetString(data);
                            if (!text.StartsWith("PJS/1")) continue;
                            var kv = new Dictionary<string, string>();
                            foreach (string raw in text.Split('\n'))
                            {
                                string line = raw.Trim();
                                int eq = line.IndexOf('=');
                                if (eq > 0) kv[line.Substring(0, eq)] = line.Substring(eq + 1);
                            }
                            var d = new Device();
                            if (kv.ContainsKey("name")) d.Name = kv["name"];
                            if (kv.ContainsKey("mod")) d.Model = kv["mod"];
                            // 以广播包的源地址为准：电视从 Wi-Fi 换到网线后，包里的 ip= 可能还是旧地址，
                            // 而源地址一定是它当前的真实地址（这是「换 IP 后搜不到/连不上」的根因）。
                            d.Ip = ep.Address.ToString();
                            int p;
                            if (kv.ContainsKey("port") && int.TryParse(kv["port"], out p)) d.Port = p;
                            lock (_devices)
                            {
                                // 同一台电视（按名称）只保留最新 IP，避免下拉框里留着失效的旧地址
                                var stale = new List<string>();
                                foreach (var kvp in _devices)
                                    if (kvp.Value.Name == d.Name && kvp.Key != d.Ip) stale.Add(kvp.Key);
                                foreach (var k2 in stale) _devices.Remove(k2);
                                _devices[d.Ip] = d;
                            }
                            var snapshot = new List<Device>();
                            lock (_devices) snapshot.AddRange(_devices.Values);
                            if (onFound != null) onFound(snapshot);
                        }
                    }
                    catch (Exception) { }
                    finally
                    {
                        if (_udp != null) try { _udp.Close(); } catch { }
                        _udp = null;
                    }
                    if (!_run) break;
                    Thread.Sleep(2000);
                }
            });
            _thread.IsBackground = true;
            _thread.Start();
        }

        public void Stop()
        {
            _run = false;
            try { if (_udp != null) _udp.Close(); } catch { }
            try { if (_thread != null) _thread.Join(500); } catch { }
        }

        public void Restart(FoundHandler onFound)
        {
            Stop();
            lock (_devices) _devices.Clear();
            Start(onFound);
        }
    }

class LogoBox : Panel
{
    public LogoBox() { DoubleBuffered = true; BackColor = Color.Transparent; }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        g.SmoothingMode = SmoothingMode.AntiAlias;
        var r = new Rectangle(0, 0, Width - 1, Height - 1);
        var img = MainForm.Logo;
        using (var p = T.Round(r, 9))
        {
            if (img != null)
            {
                var old = g.Clip;
                g.SetClip(p);
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.DrawImage(img, r);
                g.Clip = old;
            }
            else
            {
                using (var b = new LinearGradientBrush(r, T.Accent, T.Cyan, 45f)) g.FillPath(b, p);
                using (var f = T.F(18, true))
                using (var b = new SolidBrush(Color.White))
                    g.DrawString("▶", f, b, 7, 5);
            }
        }
    }
}


    /// <summary>关于窗口：与主界面同一套主题的自绘对话框。</summary>
    class AboutForm : Form
    {
        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr h, int a, ref int v, int s);
        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int m, IntPtr w, IntPtr l);

        public AboutForm()
        {
            Text = "关于 星联投屏";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = T.Sz(420, 380);
            BackColor = T.Card;
            ShowInTaskbar = false;
            AutoScaleMode = AutoScaleMode.None;

            var logo = new LogoBox();
            logo.Location = T.Pt(20, 18);
            logo.Size = T.Sz(46, 46);

            var name = new TitleBox();
            name.Text = "星联投屏";
            name.Location = T.Pt(78, 12);
            name.Size = T.Sz(200, 42);

            var ver = new Label2();
            ver.Text = "v1.0.19  Windows → 智慧屏 局域网投屏";
            ver.Font = T.F(12);
            ver.ForeColor = T.Sub;
            ver.Location = T.Pt(80, 52);
            ver.Size = T.Sz(320, 20);

            var body = new Label2();
            body.Text = "引擎  XingLianCastSender.exe\n" +
                        "传输  UDP 45690（视频/音频）\n" +
                        "发现  UDP 45691 · 回传 45692\n" +
                        "字体  Noto Sans SC 子集（SIL OFL 1.1，全文见 OFL.txt）";
            body.Font = T.F(12);
            body.ForeColor = T.Text;
            body.Location = T.Pt(22, 92);
            body.Size = T.Sz(376, 92);

            // ---- 联系我们 ----
            var contactCap = new Label2();
            contactCap.Text = "联系我们";
            contactCap.Font = T.F(12.5f, true);
            contactCap.ForeColor = T.Text;
            contactCap.Location = T.Pt(22, 192);
            contactCap.Size = T.Sz(200, 20);

            var contact = new Label2();
            contact.Text = "E-Mail   xujinq@hotmail.com\n" +
                           "Mobile   +86 13996351705";
            contact.Font = T.F(12.5f);
            contact.ForeColor = T.Accent;
            contact.Location = T.Pt(22, 216);
            contact.Size = T.Sz(376, 44);
            contact.Cursor = Cursors.Hand;
            contact.Click += delegate
            {
                try { Clipboard.SetText("xujinq@hotmail.com  /  +86 13996351705"); } catch { }
            };

            var price = new Label2();
            price.Text = "版权费用：10 元/年　·　58 元终生\n" +
                         "版权所有，请通过 E-Mail 或电话购买授权";
            price.Font = T.F(12);
            price.ForeColor = T.Sub;
            price.Location = T.Pt(22, 262);
            price.Size = T.Sz(376, 44);

            var ok = new FlatButton();
            ok.Text = "确定";
            ok.Size = T.Sz(96, 36);
            ok.Location = T.Pt(420 - 96 - 22, 380 - 36 - 18);
            ok.Fill = T.Accent; ok.FillHover = T.AccentHi; ok.TextColor = Color.White;
            ok.Click += delegate { Close(); };

            var copy = new FlatButton();
            copy.Text = "复制联系方式";
            copy.Size = T.Sz(120, 36);
            copy.Location = T.Pt(420 - 96 - 22 - 132, 380 - 36 - 18);
            copy.Fill = T.Card2; copy.FillHover = T.Hover; copy.TextColor = T.Text;
            copy.Click += delegate
            {
                try { Clipboard.SetText("xujinq@hotmail.com  /  +86 13996351705"); } catch { }
                copy.Text = "✓ 已复制";
                var t = new System.Windows.Forms.Timer();
                t.Interval = 1500;
                t.Tick += delegate { t.Stop(); t.Dispose(); copy.Text = "复制联系方式"; };
                t.Start();
            };

            Controls.Add(logo);
            Controls.Add(name);
            Controls.Add(ver);
            Controls.Add(body);
            Controls.Add(contactCap);
            Controls.Add(contact);
            Controls.Add(price);
            Controls.Add(copy);
            Controls.Add(ok);

            MouseDown += DragIt;
            foreach (Control ctl in Controls)
                if (!(ctl is Button)) ctl.MouseDown += DragIt;   // 按钮要留给点击，不能用来拖动

            KeyPreview = true;
            KeyDown += delegate(object s, KeyEventArgs e) { if (e.KeyCode == Keys.Escape) Close(); };
            DoubleBuffered = true;
            Paint += delegate(object s, PaintEventArgs e)
            {
                e.Graphics.SmoothingMode = SmoothingMode.AntiAlias;
                using (var p = new Pen(T.Border)) e.Graphics.DrawRectangle(p, 0, 0, Width - 1, Height - 1);
                using (var pen = new Pen(T.Hairline))
                    e.Graphics.DrawLine(pen, T.I(20), T.I(184), Width - T.I(20), T.I(184));
            };
        }

        void DragIt(object s, MouseEventArgs e)
        {
            if (e.Button != MouseButtons.Left) return;
            ReleaseCapture();
            SendMessage(Handle, 0xA1, (IntPtr)2, IntPtr.Zero);
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try { int v = 2; DwmSetWindowAttribute(Handle, 33, ref v, sizeof(int)); } catch { }
        }
    }

    // ---------------- main window ----------------
    class MainForm : Form
    {
        [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr OpenEventW(uint access, bool inherit, string name);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetEvent(IntPtr h);
        [DllImport("kernel32.dll", SetLastError = true)] static extern bool CloseHandle(IntPtr h);
        const uint EVENT_MODIFY_STATE = 0x0002;

        [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int val, int size);
        [DllImport("user32.dll")] static extern uint GetDpiForSystem();

        static Image _logo;
        public static Image Logo
        {
            get
            {
                if (_logo == null)
                {
                    try
                    {
                        var asm = typeof(MainForm).Assembly;
                        using (var s = asm.GetManifestResourceStream("logo.png"))
                            if (s != null) _logo = Image.FromStream(s);
                    }
                    catch { }
                }
                return _logo;
            }
        }

        readonly string _baseDir;
        readonly string _exePath;

        // header
        Panel _header;
        GlyphButton _btnMenu, _btnMin, _btnClose;
        // main
        ComboBox _cboMonitor, _cboHeight, _cboFps, _cboBitrate, _cboLogSize, _cboDevice;
        FlatButton _btnRefresh, _btnAutoRecover;
        bool _autoRecover = true;      // 断线自动恢复（锁屏/断网/引擎退出都算）
        bool _wantCast;                // "我要投屏"的意图：只有用户点停止才清除
        System.Windows.Forms.Timer _retryTimer;
        System.Windows.Forms.Timer _trialTimer;   // 未注册时按秒扣减当日试用额度
        Label2 _lblUnregistered;                   // 标题旁的「未注册 · 剩余 m:ss」徽标
        System.Windows.Forms.Timer _badgeTimer;    // 徽标每秒刷新
        int _retryDelay = 2000;        // 重启退避：2s → 4s → 8s → 10s
        bool _switchedNotice;          // 本次启动是因为被投显示器消失→已切主屏
        Label2 _lblDeviceHint;   // 接收端右侧：未发现电视端时给出提示
        System.Windows.Forms.Timer _refreshTimer;   // 「搜索中…」状态的超时恢复
        bool _searching;                            // 正在搜索：此时不显示「未发现」红字
        
        AccentButton _btnStart;
        FlatButton _btnStop;
        Card _cardMain;
        // status / footer
        Label _lblFootL, _lblFootC, _lblFootR;
        string _stamp = "?";
        readonly MetricCard[] _mv = new MetricCard[10];

        // drawers
        ToolStripMenuItem _miAutostart;
        FlatButton _btnAudio;
        ContextMenuStrip _menu;
        bool _audioOn = true;
        // runtime
        Process _proc;
        readonly Discovery _discovery = new Discovery();
        Discovery.FoundHandler _onFound;
        bool _closing;
        bool _trayHinted;
        string _stateText = "开始投屏";
        long _lastTvLost = -1;
        int _stateColor = 0;

        public MainForm()
        {
            _baseDir = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
            _exePath = Path.Combine(_baseDir, "XingLianCastSender.exe");

            string stamp;
            try { stamp = File.GetLastWriteTime(Application.ExecutablePath).ToString("MM-dd HH:mm"); }
            catch { stamp = "?"; }

            Text = "星联投屏";
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.CenterScreen;
            string th = Environment.GetEnvironmentVariable("PJS_UI_THEME");
            if (th != null) T.Light = !th.Trim().ToLowerInvariant().StartsWith("d");   // 默认浅色
            // 取系统 DPI：优先 GetDpiForSystem()（PMv2 下可靠），退回 Graphics.FromHwnd
            uint sysDpi = 0;
            try { sysDpi = GetDpiForSystem(); } catch { }
            if (sysDpi < 96 || sysDpi > 480)
            {
                using (var g0 = Graphics.FromHwnd(IntPtr.Zero)) sysDpi = (uint)Math.Round(g0.DpiX);
            }
            if (sysDpi >= 96) T.S = sysDpi / 96f;
            AutoScaleMode = AutoScaleMode.None;
            ClientSize = T.Sz(780, 540);
            MinimumSize = T.Sz(720, 520);
            BackColor = T.Bg1;
            Font = T.F(13);
            DoubleBuffered = true;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw, true);
            KeyPreview = true;

            try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
            LoadUiConfig();
            BuildUi(stamp);
            RefreshLicenseBadge();
            if (_badgeTimer == null)
            {
                _badgeTimer = new System.Windows.Forms.Timer();
                _badgeTimer.Interval = 1000;
                _badgeTimer.Tick += delegate { RefreshLicenseBadge(); };
            }
            _badgeTimer.Start();
            ApplyAutoRecoverStyle();
            LoadMonitors();
            LoadAutostartState();
            RefreshFooter();
            StartDiscovery();
            UpdateButtons();
            SetupTray();

            FormClosing += delegate(object s, FormClosingEventArgs e)
            {
                if (!_closing)
                {
                    e.Cancel = true;
                    if (_trayAdded)
                    {
                        Hide();                 // 有托盘图标：收进托盘
                    }
                    else
                    {
                        // 没有托盘图标：最小化到任务栏，保证还能找回来
                        WindowState = FormWindowState.Minimized;
                        FlashStatus("已最小化到任务栏（本机无法创建托盘图标）");
                    }
                    return;
                }
                StopSender();
                _discovery.Stop();
                TrayRemove();
            };
            KeyDown += delegate(object s, KeyEventArgs e) { if (e.KeyCode == Keys.Escape && _menu != null && _menu.Visible) _menu.Close(); };

            // 用 Shown 而不是 BeginInvoke：构造期窗口句柄还不存在，BeginInvoke 会抛异常
            foreach (string a in Environment.GetCommandLineArgs())
            {
                if (a == "--start") Shown += delegate { StartSender(); };
                if (a == "--about") Shown += delegate { using (var d = new AboutForm()) d.ShowDialog(this); };
            }
        }

        [DllImport("user32.dll")] static extern bool ReleaseCapture();
        [DllImport("user32.dll")] static extern IntPtr SendMessage(IntPtr h, int msg, IntPtr w, IntPtr l);

        /// <summary>无边框窗口：让标题栏区域（含子控件）都能拖动窗口。</summary>
        void MakeDraggable(Control ctl)
        {
            if (ctl == null) return;
            ctl.MouseDown += delegate(object s, MouseEventArgs e)
            {
                if (e.Button != MouseButtons.Left) return;
                ReleaseCapture();
                SendMessage(Handle, 0xA1 /*WM_NCLBUTTONDOWN*/, (IntPtr)2 /*HTCAPTION*/, IntPtr.Zero);
            };
        }

        /// <summary>☰ 菜单：开机自启 / 日志 / 关于 / 退出。</summary>
        void ShowMenu()
        {
            _menu.Show(_btnMenu, new Point(0, _btnMenu.Height + T.I(4)));
        }

        class MenuColors : ProfessionalColorTable
        {
            public override Color MenuItemSelected { get { return T.Hover; } }
            public override Color MenuItemSelectedGradientBegin { get { return T.Hover; } }
            public override Color MenuItemSelectedGradientEnd { get { return T.Hover; } }
            public override Color MenuItemBorder { get { return T.Hover; } }
            public override Color ToolStripDropDownBackground { get { return T.Card; } }
            public override Color ImageMarginGradientBegin { get { return T.Card; } }
            public override Color ImageMarginGradientMiddle { get { return T.Card; } }
            public override Color ImageMarginGradientEnd { get { return T.Card; } }
            public override Color SeparatorDark { get { return T.Hairline; } }
            public override Color SeparatorLight { get { return T.Hairline; } }
        }

        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x02000000;                  // WS_EX_COMPOSITED：整窗离屏合成
                cp.Style |= 0x00020000 | 0x00080000;       // WS_MINIMIZEBOX | WS_SYSMENU
                return cp;
            }
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
        }

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try { int v = 2; DwmSetWindowAttribute(Handle, 33, ref v, sizeof(int)); } catch { }   // 圆角
            if (!_trayAdded) TryTrayAdd(0);                                                     // 托盘图标（失败会重试）
        }

        protected override void WndProc(ref Message m)
        {
            const int WM_NCHITTEST = 0x84, HTCAPTION = 2, HTCLIENT = 1, WM_ERASEBKGND = 0x14;
            const int WM_SYSCOMMAND = 0x112, SC_RESTORE = 0xF120, SC_MINIMIZE = 0xF020;
            if (m.Msg == WM_ERASEBKGND) { m.Result = (IntPtr)1; return; }   // 不让系统擦背景，避免闪白
            if (m.Msg == WM_SYSCOMMAND)
            {
                int cmd = (int)((long)m.WParam & 0xFFF0);
                if (cmd == SC_RESTORE) { RestoreFromTray(); return; }   // 任务栏双击还原

            }
            if (m.Msg == WM_TRAYMSG && m.HWnd == Handle)
            {
                int ev = (int)((long)m.LParam & 0xFFFF);
                if (ev == 0x202) RestoreFromTray();                       // 左键抬起
                else if (ev == 0x205)                                     // 右键抬起
                {
                    if (_menu != null) _menu.Show(Cursor.Position);
                }
                return;
            }
            base.WndProc(ref m);
            if (m.Msg == WM_NCHITTEST && m.Result == (IntPtr)HTCLIENT && _header != null)
            {
                Point p = PointToClient(new Point((short)(long)m.LParam, (short)((long)m.LParam >> 16)));
                if (p.Y < _header.Height) m.Result = (IntPtr)HTCAPTION;
            }
        }

        /// <summary>给任意控件打开双缓冲（TableLayoutPanel 默认不开，恢复窗口时会闪）。</summary>
        static void EnableDoubleBuffer(Control ctl)
        {
            if (ctl == null) return;
            try
            {
                var pi = typeof(Control).GetProperty("DoubleBuffered",
                    System.Reflection.BindingFlags.Instance | System.Reflection.BindingFlags.NonPublic);
                if (pi != null) pi.SetValue(ctl, true, null);
            }
            catch { }
        }

        // ---------------- UI construction ----------------
        TableLayoutPanel _root;

        void BuildUi(string stamp)
        {
            _stamp = stamp;
            _root = new TableLayoutPanel();
            _root.Dock = DockStyle.Fill;
            _root.ColumnCount = 1;
            _root.RowCount = 6;
            _root.BackColor = T.Bg1;
            _root.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(62)));   // header
            _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 0));       // settings drawer
            _root.RowStyles.Add(new RowStyle(SizeType.Percent, 100));      // main
            _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 0));       // log drawer
            _root.RowStyles.Add(new RowStyle(SizeType.Absolute, 0));         // status（已并入按钮）
            _root.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(38)));   // footer
            EnableDoubleBuffer(_root);
            Controls.Add(_root);

            // ---- header ----
            _header = new BufPanel();
            _header.Dock = DockStyle.Fill;
            _header.Margin = new Padding(0);
            _header.BackColor = T.HeaderBg;
            _root.Controls.Add(_header, 0, 0);

            var logo = new LogoBox();
            logo.Location = T.Pt(18, 9);
            logo.Size = T.Sz(44, 44);
            _header.Controls.Add(logo);
            MakeDraggable(logo);

            var title = new TitleBox();
            title.Text = "星联投屏";
            int titleW;
            using (var tf = T.Title(25)) titleW = TextRenderer.MeasureText("星联投屏", tf).Width;
            title.Location = T.Pt(74, 0);
            title.Size = T.Sz(titleW / T.S + 10, 62);
            _header.Controls.Add(title);

            // 未注册徽标：标题右侧（点击打开注册窗口，每秒刷新剩余试用）
            _lblUnregistered = new Label2();
            _lblUnregistered.Font = T.F(11.5f);
            _lblUnregistered.ForeColor = Color.FromArgb(0xE0, 0x7A, 0x3C);
            _lblUnregistered.Location = T.Pt(74 + titleW / T.S + 16, 0);
            _lblUnregistered.Size = T.Sz(210, 62);
            _lblUnregistered.TextAlign = ContentAlignment.MiddleLeft;
            _lblUnregistered.Cursor = Cursors.Hand;
            _lblUnregistered.Text = "";
            _lblUnregistered.Click += delegate { OpenLicenseDialog(); };
            _header.Controls.Add(_lblUnregistered);
            MakeDraggable(title);


            _btnClose = IconButton(Glyph.Close, true);
            _btnMin = IconButton(Glyph.Min, false);
            _btnMenu = IconButton(Glyph.Menu, false);
            _btnMenu.Click += delegate { ShowMenu(); };
            _btnMin.Click += delegate { WindowState = FormWindowState.Minimized; };
            _btnClose.Click += delegate { _closing = true; Close(); };
            _header.Controls.Add(_btnMenu);
            _header.Controls.Add(_btnMin);
            _header.Controls.Add(_btnClose);
            // 分隔线：画在内容区顶部（顶栏内的子控件会覆盖顶栏自身的绘制，导致断线）
            
            _header.Resize += delegate { LayoutHeaderButtons(); };
            LayoutHeaderButtons();
            MakeDraggable(_header);
            // ---- footer ----
            var footer = new BufPanel();
            footer.Dock = DockStyle.Fill;
            footer.Margin = new Padding(0);
            footer.BackColor = T.FooterBg;
            _root.Controls.Add(footer, 0, 5);
            var ft = new TableLayoutPanel();
            ft.Dock = DockStyle.Fill;
            ft.ColumnCount = 3;
            EnableDoubleBuffer(ft);
            ft.RowCount = 1;
            ft.BackColor = Color.Transparent;
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 56));
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 12));
            ft.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 32));
            _lblFootL = FootLabel(ContentAlignment.MiddleLeft, 18, 0);
            _lblFootC = FootLabel(ContentAlignment.MiddleCenter, 0, 0);
            _lblFootR = FootLabel(ContentAlignment.MiddleRight, 0, 18);
            _lblFootR.AutoEllipsis = false;
            ft.Controls.Add(_lblFootL, 0, 0);
            ft.Controls.Add(_lblFootC, 1, 0);
            ft.Controls.Add(_lblFootR, 2, 0);
            footer.Controls.Add(ft);
            var footerLine = new Panel();
            footerLine.Dock = DockStyle.Top;
            footerLine.Height = 1;
            footerLine.BackColor = T.Hairline;
            footer.Controls.Add(footerLine);

            // ---- main area ----
            var main = new GradientPanel();
            main.Dock = DockStyle.Fill;
            main.Margin = new Padding(0);
            main.Padding = T.Pad(16, 14, 16, 12);
            _root.Controls.Add(main, 0, 2);
            main.Paint += delegate(object s, PaintEventArgs e)
            {
                using (var pen = new Pen(T.Hairline))
                    e.Graphics.DrawLine(pen, 0, 0, main.Width, 0);
            };

            _cardMain = new Card();
            _cardMain.Dock = DockStyle.Top;
            _cardMain.Height = T.I(404);
            main.Controls.Add(_cardMain);

            var ctl = new BufPanel();
            ctl.Dock = DockStyle.Fill;
            ctl.BackColor = Color.Transparent;
            ctl.Padding = T.Pad(22, 18, 22, 16);
            _cardMain.Controls.Add(ctl);

            var grid = new TableLayoutPanel();
            grid.Dock = DockStyle.Top;
            grid.AutoSize = false;
            grid.Height = T.I(44 * 4 + 72 + 128);
            grid.BackColor = Color.Transparent;
            grid.ColumnCount = 4;
            grid.RowCount = 6;
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, T.I(72)));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 56));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, T.I(92)));
            grid.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 44));
            for (int i = 0; i < 4; i++) grid.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(44)));
            grid.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(72)));
            grid.RowStyles.Add(new RowStyle(SizeType.Absolute, T.I(128)));  // 指标块（2 行）
            EnableDoubleBuffer(grid);
            ctl.Controls.Add(grid);

            grid.Controls.Add(Txt("显示器"), 0, 0);
            _cboMonitor = Combo();
            grid.Controls.Add(_cboMonitor, 1, 0);
            grid.SetColumnSpan(_cboMonitor, 3);

            grid.Controls.Add(Txt("分辨率"), 0, 1);
            _cboHeight = Combo();
            _cboHeight.Items.AddRange(new object[] { "原生分辨率", "1440p (2560x1440)", "1080p (1920x1080)", "720p (1280x720)" });
            _cboHeight.SelectedIndex = 0;
            grid.Controls.Add(_cboHeight, 1, 1);

            grid.Controls.Add(Txt("帧率"), 2, 1);
            _cboFps = Combo();
            _cboFps.Items.AddRange(new object[] { "60", "30" });
            _cboFps.SelectedIndex = 0;
            grid.Controls.Add(_cboFps, 3, 1);

            grid.Controls.Add(Txt("码率"), 0, 2);
            _cboBitrate = Combo();
            _cboBitrate.Items.AddRange(new object[] { "4000", "6000", "8000", "12000", "16000", "20000" });
            _cboBitrate.SelectedIndex = 2;
            grid.Controls.Add(_cboBitrate, 1, 2);

            grid.Controls.Add(Txt("日志上限"), 2, 2);
            _cboLogSize = Combo();
            _cboLogSize.Items.AddRange(new object[] { "5 MB", "10 MB", "20 MB", "50 MB", "100 MB" });
            _cboLogSize.SelectedIndex = 0;
            grid.Controls.Add(_cboLogSize, 3, 2);

            grid.Controls.Add(Txt("接收端"), 0, 3);
            _cboDevice = Combo();
            grid.Controls.Add(_cboDevice, 1, 3);

            _btnRefresh = new FlatButton();
            _btnRefresh.Text = "刷新";
            _btnRefresh.Fill = T.Card2; _btnRefresh.FillHover = T.Border;
            _btnRefresh.Height = T.I(32);
            _btnRefresh.Dock = DockStyle.Left;
            _btnRefresh.Width = T.I(88);
            _btnRefresh.Margin = T.Pad(6, 6, 0, 6);
            _btnRefresh.Click += delegate { RefreshDevices(); };

            grid.Controls.Add(_btnRefresh, 2, 3);
            grid.SetColumnSpan(_btnRefresh, 1);

            // 未发现 TV 的提示：放在「刷新」右侧，与接收端下拉同一行
            _lblDeviceHint = new Label2();
            _lblDeviceHint.Text = "未发现电视端，请确认开启后重试";
            _lblDeviceHint.Font = T.F(12);
            _lblDeviceHint.ForeColor = T.Err;
            _lblDeviceHint.Dock = DockStyle.Fill;
            _lblDeviceHint.Margin = T.Pad(8, 0, 0, 0);
            _lblDeviceHint.TextAlign = ContentAlignment.MiddleLeft;
            _lblDeviceHint.Visible = true;      // 启动即提示，直到发现 TV 才隐藏
            grid.Controls.Add(_lblDeviceHint, 3, 3);

            var btnRow = new FlowLayoutPanel();
            btnRow.Dock = DockStyle.Fill;
            btnRow.FlowDirection = FlowDirection.LeftToRight;
            EnableDoubleBuffer(btnRow);
            btnRow.BackColor = Color.Transparent;
            btnRow.Margin = T.Pad(0, 10, 0, 0);
            _btnStart = new AccentButton();
            _btnStart.Text = "开始投屏";
            _btnStart.Size = T.Sz(150, 42);
            _btnStart.Margin = T.Pad(0, 0, 10, 0);
            _btnStart.Click += delegate { StartSender(); };
            _btnStop = new FlatButton();
            _btnStop.Text = "停止投屏";
            _btnStop.Size = T.Sz(150, 42);
            _btnStop.Margin = T.Pad(0, 0, 10, 0);
            _btnStop.Fill = T.StopFill; _btnStop.FillHover = T.Hover;
            _btnStop.Stroke = T.StopStroke;
            _btnStop.TextColor = T.StopText;
            _btnStop.Click += delegate { StopSender(); };
            _btnAudio = new FlatButton();
            _btnAudio.Text = "声音：开";
            _btnAudio.Size = T.Sz(150, 42);
            _btnAudio.Margin = T.Pad(0, 0, 10, 0);
            ApplyAudioStyle();
            _btnAudio.Click += delegate { ToggleAudio(); };

            _btnAutoRecover = new FlatButton();
            _btnAutoRecover.Size = T.Sz(150, 42);
            _btnAutoRecover.Margin = T.Pad(0, 0, 10, 0);
            ApplyAutoRecoverStyle();
            _btnAutoRecover.Click += delegate
            {
                _autoRecover = !_autoRecover;
                SaveUiConfig();
                ApplyAutoRecoverStyle();
                if (_autoRecover && _wantCast && _proc == null) StartSender();
            };

            btnRow.Controls.Add(_btnStart);
            btnRow.Controls.Add(_btnStop);
            btnRow.Controls.Add(_btnAudio);
            btnRow.Controls.Add(_btnAutoRecover);
            grid.Controls.Add(btnRow, 0, 4);
            grid.SetColumnSpan(btnRow, 4);

            // ---- 指标卡片（2 行 x 5 列）----
            var meters = new TableLayoutPanel();
            meters.Dock = DockStyle.Fill;
            meters.Margin = T.Pad(0, 8, 0, 0);
            meters.BackColor = Color.Transparent;
            meters.ColumnCount = 5;
            meters.RowCount = 2;
            EnableDoubleBuffer(meters);
            for (int i = 0; i < 5; i++) meters.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 20));
            meters.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
            meters.RowStyles.Add(new RowStyle(SizeType.Percent, 50));
            var titles = new[]
            {
                "电脑帧率", "编码耗时/帧", "音频带宽", "网络流量", "电脑丢包/秒",
                "电视帧率", "采集卡顿", "限帧丢弃", "网络延迟", "电视丢包/秒"
            };
            for (int i = 0; i < 10; i++)
            {
                var card = new MetricCard();
                card.Caption = titles[i];
                card.Dock = DockStyle.Fill;
                card.Margin = T.Pad(0, 0, 6, 6);
                _mv[i] = card;
                meters.Controls.Add(card, i % 5, i / 5);
            }
            grid.Controls.Add(meters, 0, 5);
            grid.SetColumnSpan(meters, 4);
            SetState("未连接", 0);
            if (Environment.GetEnvironmentVariable("PJS_UI_DEBUG") != null)
                Shown += delegate { DumpLayout(); };
        }

        void DumpLayout()
        {
            var sb = new StringBuilder();
            sb.AppendLine("form client=" + ClientSize);
            sb.AppendLine("root=" + _root.Bounds);
            for (int i = 0; i < _root.RowCount; i++) sb.AppendLine("row" + i + " h=" + _root.RowStyles[i].Height + " type=" + _root.RowStyles[i].SizeType);
            sb.AppendLine("header=" + _header.Bounds + " visible=" + _header.Visible);

            foreach (Control c in _root.Controls) sb.AppendLine("child " + c.GetType().Name + " " + c.Bounds + " vis=" + c.Visible);
            foreach (Control c in _header.Controls) sb.AppendLine("hdr " + c.GetType().Name + " text=\x27" + c.Text + "\x27 " + c.Bounds + " vis=" + c.Visible);
            File.WriteAllText(Path.Combine(_baseDir, "ui_layout.txt"), sb.ToString());
        }

        GlyphButton IconButton(Glyph kind, bool danger)
        {
            var b = new GlyphButton();
            b.Kind = kind;
            b.Size = T.Sz(34, 30);
            b.FillHover = danger ? (T.Light ? Color.FromArgb(255, 232, 231) : Color.FromArgb(120, 60, 90)) : T.Hover;
            b.Anchor = AnchorStyles.Top | AnchorStyles.Right;
            return b;
        }        void LayoutHeaderButtons()
        {
            int top = T.I(15), gap = T.I(6), margin = T.I(14);
            int x = _header.Width - margin;
            foreach (var b in new Control[] { _btnClose, _btnMin, _btnMenu })
            {
                b.Left = x - b.Width;
                b.Top = top;
                x -= b.Width + gap;
            }
        }

        Label Txt(string s)
        {
            var l = new Label2();
            l.Text = s;
            l.Dock = DockStyle.Fill;
            l.Margin = new Padding(0, 0, 8, 0);
            return l;
        }

        ComboBox Combo()
        {
            var c = new ComboBox();
            c.DropDownStyle = ComboBoxStyle.DropDownList;
            c.FlatStyle = FlatStyle.Flat;
            c.BackColor = T.Card2;
            c.ForeColor = T.Text;
            c.Font = T.F(13);
            c.Dock = DockStyle.Fill;
            c.Margin = T.Pad(0, 6, 10, 6);
            c.DrawMode = DrawMode.OwnerDrawFixed;
            c.ItemHeight = T.I(26);
            // 选中后取消焦点：Flat 样式的 ComboBox 在获得焦点时会被系统刷成淡蓝，
            // 只有把焦点移走才会恢复常态（这不是"选中项残留"）。
            c.DropDownClosed += delegate
            {
                if (!c.IsHandleCreated) return;
                try
                {
                    c.BeginInvoke((Action)delegate
                    {
                        ActiveControl = null;
                        c.BackColor = T.Card2;
                        c.ForeColor = T.Text;
                        c.Invalidate();
                    });
                }
                catch { }
            };
            c.SelectedIndexChanged += delegate
            {
                c.BackColor = T.Card2;
                c.ForeColor = T.Text;
            };
            c.DrawItem += delegate(object s, DrawItemEventArgs e)
            {
                if (e.Index < 0) return;
                bool sel = (e.State & DrawItemState.Selected) != 0;
                var b = e.Bounds;
                b.Inflate(-1, 0);
                using (var br = new SolidBrush(sel ? T.SelectBg : T.Card2)) e.Graphics.FillRectangle(br, b);
                TextRenderer.DrawText(e.Graphics, c.Items[e.Index].ToString(), c.Font, b,
                                      sel ? T.SelectFg : T.Text,
                                      TextFormatFlags.Left | TextFormatFlags.VerticalCenter);
            };
            return c;
        }

        // ---------------- status helpers ----------------
        /// <summary>状态直接体现在「开始投屏」按钮上：蓝=待机、橙=启动中、绿=投屏中、红=错误。</summary>
        void SetState(string text, int color)
        {
            _stateText = text;
            _stateColor = color;
            ApplyButtonState();
        }

        void ApplyButtonState()
        {
            if (_btnStart == null) return;
            switch (_stateColor)
            {
                case 1:   // 投屏中
                    _btnStart.Text = _stateText.Length > 0 ? _stateText : "投屏中";
                    _btnStart.Base = T.Ok;
                    _btnStart.Base2 = T.Light ? Color.FromArgb(48, 219, 91) : Color.FromArgb(52, 199, 89);
                    _btnStart.Cursor = Cursors.Default;
                    break;
                case 2:   // 启动中
                    _btnStart.Text = _stateText.Length > 0 ? _stateText : "启动中…";
                    _btnStart.Base = T.Warn;
                    _btnStart.Base2 = T.Light ? Color.FromArgb(255, 169, 47) : T.Warn;
                    _btnStart.Cursor = Cursors.Default;
                    break;
                case 3:   // 错误：主按钮临时变红显示原因（底栏不再打印任何提示）
                    _btnStart.Text = _stateText.Length > 0 ? _stateText : "出错了";
                    _btnStart.Base = T.Err;
                    _btnStart.Base2 = Color.FromArgb(0xD9, 0x3A, 0x3A);
                    _btnStart.Cursor = Cursors.Default;
                    break;
                default:
                    _btnStart.Text = "开始投屏";
                    _btnStart.Base = T.Accent;
                    _btnStart.Base2 = T.AccentHi;
                    _btnStart.Cursor = Cursors.Hand;
                    break;
            }
            _btnStart.Invalidate();
        }

        /// <summary>状态提示：只写日志文件，不在界面上打印（底栏保持静态）。</summary>
        void FlashStatus(string msg)
        {
            if (string.IsNullOrEmpty(msg)) return;
            Log("[status] " + msg);
        }

        // ---------------- data ----------------
        void LoadMonitors()
        {
            _cboMonitor.Items.Clear();
            try { foreach (var m in Monitors.List()) _cboMonitor.Items.Add(m); }
            catch (Exception ex) { Log("枚举显示器失败: " + ex.Message); }
            if (_cboMonitor.Items.Count > 0) _cboMonitor.SelectedIndex = 0;
        }

        Label FootLabel(ContentAlignment align, int padLeft, int padRight)
        {
            var l = new Label2();
            l.TextAlign = align;
            l.Dock = DockStyle.Fill;
            l.ForeColor = T.FooterText;
            l.Font = T.F(12);
            l.BackColor = Color.Transparent;
            l.Margin = new Padding(0);
            l.Padding = T.Pad(padLeft, 0, padRight, 0);
            return l;
        }

        void RefreshFooter()
        {
            string ip = "—";
            try
            {
                foreach (var ni in NetworkInterface.GetAllNetworkInterfaces())
                {
                    if (ni.OperationalStatus != OperationalStatus.Up) continue;
                    if (ni.NetworkInterfaceType == NetworkInterfaceType.Loopback) continue;
                    foreach (var ua in ni.GetIPProperties().UnicastAddresses)
                    {
                        if (ua.Address.AddressFamily != AddressFamily.InterNetwork) continue;
                        ip = ua.Address.ToString();
                        break;
                    }
                    if (ip != "—") break;
                }
            }
            catch { }
            if (_lblFootL != null)
                _lblFootL.Text = "计算机名: " + Environment.MachineName + "          IP: " + ip;
            if (_lblFootC != null) _lblFootC.Text = "";
            if (_lblFootR != null) _lblFootR.Text = "v1.0.19  ·  build " + _stamp;
        }

        void LoadAutostartState()
        {
            try
            {
                if (_miAutostart == null) return;
                string startup = Environment.GetFolderPath(Environment.SpecialFolder.Startup);
                _miAutostart.Checked = File.Exists(Path.Combine(startup, "XingLianCast.lnk"));
                UpdateAutostartText();
            }
            catch { }
        }

        void ApplyAutostart()
        {
            try
            {
                var psi = new ProcessStartInfo("powershell",
                    "-NoProfile -ExecutionPolicy Bypass -File \"" + Path.Combine(_baseDir, "autostart.ps1") + "\" " +
                    (_miAutostart.Checked ? "enable" : "disable"));
                psi.UseShellExecute = false; psi.CreateNoWindow = true; psi.RedirectStandardOutput = true;
                Process.Start(psi).WaitForExit(4000);
            }
            catch (Exception ex) { Log("设置开机自启失败: " + ex.Message); }
        }

        /// <summary>自动恢复按钮的外观：开=绿色，关=灰色。</summary>
        void ApplyAutoRecoverStyle()
        {
            if (_btnAutoRecover == null) return;
            _btnAutoRecover.Text = _autoRecover ? "自动恢复：开" : "自动恢复：关";
            // 与「声音」按钮完全一致：同样的绿、同样的悬停色、同样的灰
            if (_autoRecover)
            {
                _btnAutoRecover.Fill = T.Ok;
                _btnAutoRecover.FillHover = T.Light ? Color.FromArgb(48, 219, 91) : Color.FromArgb(64, 214, 104);
                _btnAutoRecover.Stroke = Color.Transparent;
                _btnAutoRecover.TextColor = Color.White;
            }
            else
            {
                _btnAutoRecover.Fill = T.StopFill;
                _btnAutoRecover.FillHover = T.Hover;
                _btnAutoRecover.Stroke = T.StopStroke;
                _btnAutoRecover.TextColor = T.Sub;
            }
            _btnAutoRecover.Invalidate();
        }

        string UiConfigPath
        {
            get
            {
                string dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "星联投屏");
                Directory.CreateDirectory(dir);
                return Path.Combine(dir, "ui.cfg");
            }
        }

        void LoadUiConfig()
        {
            try
            {
                foreach (var ln in File.ReadAllLines(UiConfigPath))
                {
                    int eq = ln.IndexOf('=');
                    if (eq <= 0) continue;
                    string k = ln.Substring(0, eq).Trim(), v = ln.Substring(eq + 1).Trim();
                    if (k == "autoRecover") _autoRecover = (v == "1");
                }
            }
            catch { }
        }

        void SaveUiConfig()
        {
            try { File.WriteAllText(UiConfigPath, "autoRecover=" + (_autoRecover ? "1" : "0") + Environment.NewLine); }
            catch { }
        }

        /// <summary>意外中断后的自动重连：2s → 4s → 8s → 10s 退避。</summary>
        void ScheduleAutoRecover(string reason)
        {
            if (!_wantCast || !_autoRecover || _closing) return;
            Log("[auto] 计划恢复投屏：" + reason + "，" + _retryDelay + "ms 后重试");
            if (_retryTimer == null)
            {
                _retryTimer = new System.Windows.Forms.Timer();
                _retryTimer.Tick += delegate
                {
                    _retryTimer.Stop();
                    if (_closing || !_wantCast || !_autoRecover) return;
                    if (_proc != null && !_proc.HasExited) return;
                    Log("[auto] 重试开始投屏");
                    StartSender();
                };
            }
            _retryTimer.Stop();
            _retryTimer.Interval = _retryDelay;
            _retryTimer.Start();
            _retryDelay = Math.Min(10000, _retryDelay * 2);
        }

        /// <summary>今日试用已用完：提示并引导注册。</summary>
        void ShowTrialExhausted()
        {
            var r = MessageBox.Show(this,
                "今日免费试用（每天 5 分钟）已用完。\n\n注册后可无限时使用。\n现在去注册吗？",
                "星联投屏 · 试用结束", MessageBoxButtons.YesNo, MessageBoxIcon.Information);
            if (r == DialogResult.Yes) OpenLicenseDialog();
        }

        /// <summary>刷新标题旁「未注册 · 剩余 m:ss」徽标（已注册则隐藏）。</summary>
        void RefreshLicenseBadge()
        {
            if (_lblUnregistered == null) return;
            if (License.IsRegistered)
            {
                _lblUnregistered.Text = "";
                _lblUnregistered.Visible = false;
                return;
            }
            _lblUnregistered.Visible = true;
            int rem = Trial.RemainingToday;
            _lblUnregistered.Text = rem > 0
                ? "未注册 · 今日剩余 " + Trial.RemainingText()
                : "未注册 · 今日试用已用完";
            _lblUnregistered.ForeColor = rem > 0 ? Color.FromArgb(0xE0, 0x7A, 0x3C) : T.Err;
        }

        void OpenLicenseDialog()
        {
            try { using (var dlg = new LicenseForm()) dlg.ShowDialog(this); }
            catch { }
            RefreshLicenseBadge();
        }

        void StartTrialClock()
        {
            if (License.IsRegistered) return;
            if (_trialTimer == null)
            {
                _trialTimer = new System.Windows.Forms.Timer();
                _trialTimer.Interval = 1000;
                _trialTimer.Tick += delegate
                {
                    if (License.IsRegistered) { StopTrialClock(); return; }
                    Trial.Add(1);
                    int rem = Trial.RemainingToday;
                    if (rem <= 0)
                    {
                        StopTrialClock();
                        _wantCast = false;          // 不让自动恢复把它拉起来
                        StopSender(false);
                        RefreshLicenseBadge();
                        ShowTrialExhausted();
                    }
                };
            }
            _trialTimer.Start();
        }

        void StopTrialClock()
        {
            if (_trialTimer != null) _trialTimer.Stop();
        }

        /// <summary>投屏中监视：被投显示器是否还在？不在则切回主屏并通知电视。</summary>
        void CheckMonitorAlive()
        {
            if (_proc == null || _proc.HasExited) return;
            if (_cboMonitor.SelectedIndex <= 0) return;      // 主屏/未选：不处理
            try
            {
                var list = Monitors.List();
                bool still = false;
                string sel = _cboMonitor.SelectedItem != null ? _cboMonitor.SelectedItem.ToString() : "";
                foreach (var m in list) if (m.ToString() == sel) { still = true; break; }
                if (still) return;
                Log("[auto] 被投显示器已消失，回退到主显示器");
                _cboMonitor.SelectedIndex = 0;
                _switchedNotice = true;
                StopSender();
                _wantCast = true;
                StartSender();
            }
            catch { }
        }

        void ToggleAudio()
        {
            _audioOn = !_audioOn;
            ApplyAudioStyle();
            ApplyAudioToggle();
        }

        /// <summary>传输声音按钮：开=绿色（同"投屏中"），关=灰色（同"停止投屏"）。</summary>
        void ApplyAudioStyle()
        {
            if (_btnAudio == null) return;
            _btnAudio.Text = "传输声音：" + (_audioOn ? "开" : "关");
            if (_audioOn)
            {
                _btnAudio.Fill = T.Ok;
                _btnAudio.FillHover = T.Light ? Color.FromArgb(48, 219, 91) : Color.FromArgb(64, 214, 104);
                _btnAudio.Stroke = Color.Transparent;
                _btnAudio.TextColor = Color.White;
            }
            else
            {
                _btnAudio.Fill = T.StopFill;
                _btnAudio.FillHover = T.Hover;
                _btnAudio.Stroke = T.StopStroke;
                _btnAudio.TextColor = T.Sub;
            }
            _btnAudio.Invalidate();
        }

        void ApplyAudioToggle()
        {
            try
            {
                using (var mm = MemoryMappedFile.OpenExisting(@"Local\XingLianCastCtl"))
                using (var acc = mm.CreateViewAccessor())
                    acc.Write(0, _audioOn ? 1 : 0);
                Log("[UI] 传声音 -> " + (_audioOn ? "开" : "关"));
            }
            catch { }
        }

        /// <summary>日志目录：优先 exe 同级的 logs（绿色版/开发），其次 %LOCALAPPDATA%\星联投屏\logs（安装版）。</summary>
        /// <summary>引擎日志文件路径：优先 exe 同目录 logs（便携版），否则 LOCALAPPDATA，再否则 TEMP。</summary>
        string EngineLogPath()
        {
            string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            var cands = new List<string>
            {
                Path.Combine(_baseDir, @"logs\XingLianCast.log"),
                Path.Combine(lad, @"星联投屏\logs\XingLianCast.log"),
                Path.Combine(Path.GetTempPath(), @"星联投屏\logs\XingLianCast.log")
            };
            foreach (var p in cands)
            {
                try
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(p));
                    using (var fs = new FileStream(p, FileMode.Append, FileAccess.Write, FileShare.ReadWrite)) { }
                    return p;
                }
                catch { }
            }
            return cands[cands.Count - 1];
        }

        string LogDir()
        {
            try { return Path.GetDirectoryName(EngineLogPath()); } catch { }
            string lad = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
            string legacy = Path.Combine(lad, @"Projector\logs");   // XingLianCastSender.exe 的默认写入位置
            string local = Path.Combine(lad, @"星联投屏\logs");
            string side = Path.Combine(_baseDir, "logs");
            // 1) 哪里已经有 XingLianCast.log 就用哪里（最可靠）
            foreach (var d in new[] { legacy, side, local })
            {
                try { if (File.Exists(Path.Combine(d, "XingLianCast.log"))) return d; } catch { }
            }
            // 2) 还没有日志：回到 sender 的默认路径（能写就用它，保证菜单打开的就是以后的日志目录）
            try
            {
                Directory.CreateDirectory(legacy);
                string probe = Path.Combine(legacy, ".w");
                File.WriteAllText(probe, "x");
                File.Delete(probe);
                return legacy;
            }
            catch { }
            // 3) 退而求其次
            foreach (var d in new[] { side, local })
            {
                try
                {
                    Directory.CreateDirectory(d);
                    string probe = Path.Combine(d, ".w");
                    File.WriteAllText(probe, "x");
                    File.Delete(probe);
                    return d;
                }
                catch { }
            }
            return legacy;
        }

        /// <summary>菜单项文本带勾选符号，直观显示开关状态。</summary>
        void UpdateAutostartText()
        {
            if (_miAutostart == null) return;
            _miAutostart.Text = (_miAutostart.Checked ? "☑ " : "☐ ") + "开机自启";
        }

        void OpenLogFolder()
        {
            try
            {
                string dir = LogDir();
                Directory.CreateDirectory(dir);
                Process.Start("explorer.exe", dir);
            }
            catch (Exception ex) { Log("打开日志目录失败: " + ex.Message); }
        }

        // ---------------- discovery ----------------
        void StartDiscovery()
        {
            _onFound = delegate(List<Discovery.Device> devices)
            {
                if (_closing) return;
                try { BeginInvoke((Action)delegate { UpdateDeviceList(devices); }); } catch { }
            };
            _discovery.Start(_onFound);
            var hint = new System.Windows.Forms.Timer();
            hint.Interval = 10000;
            hint.Tick += delegate
            {
                hint.Stop(); hint.Dispose();
                UpdateDeviceHint();
            };
            hint.Start();
        }

        void UpdateDeviceList(List<Discovery.Device> devices)
        {
            // 记住「设备名」而不是「名字+IP」：电视换 IP 后同名设备要能自动重新选中
            var prevDev = _cboDevice.SelectedItem as Discovery.Device;
            string selName = prevDev != null ? prevDev.Name : null;
            _searching = false;                       // 已有结果（哪怕为空）
            _cboDevice.Items.Clear();
            foreach (var d in devices) _cboDevice.Items.Add(d);
            if (_cboDevice.Items.Count > 0)
            {
                EndRefresh();
                if (_proc == null || _proc.HasExited) SetState("已停止", 0);   // 清掉错误态文字
            }
            UpdateDeviceHint();
            if (_cboDevice.Items.Count == 0) return;
            int idx = 0;
            if (selName != null)
                for (int i = 0; i < _cboDevice.Items.Count; i++)
                {
                    var it = _cboDevice.Items[i] as Discovery.Device;
                    if (it != null && it.Name == selName) { idx = i; break; }
                }
            _cboDevice.SelectedIndex = idx;      // 强制跟随：IP 变了也选中同一台电视
        }

        /// <summary>接收端下拉右侧的提示：完全没找到 TV 时显示红色说明。</summary>
        void UpdateDeviceHint()
        {
            if (_lblDeviceHint == null) return;
            bool found = _cboDevice != null && _cboDevice.Items.Count > 0;
            // 搜索进行中不刷新红字（点刷新时列表会先被清空，否则会闪过"未发现"）；
            // 搜索结束后仍为空，才提示。
            bool show = !found && !_searching;
            _lblDeviceHint.Visible = show;
            if (show) _lblDeviceHint.Text = "未发现电视端，请确认开启后重试";
        }

        void RefreshDevices()
        {
            _searching = true;
            _cboDevice.Items.Clear();
            _cboDevice.Text = "搜索中…";
            UpdateDeviceHint();

            // 按钮进入「搜索中…」状态，给用户明确反馈；发现设备或 2.5 秒后恢复
            _btnRefresh.Text = "搜索中…";
            _btnRefresh.Enabled = false;
            if (_refreshTimer == null)
            {
                _refreshTimer = new System.Windows.Forms.Timer();
                _refreshTimer.Tick += delegate { EndRefresh(); };
            }
            _refreshTimer.Stop();
            _refreshTimer.Interval = 2500;
            _refreshTimer.Start();

            _discovery.Restart(_onFound);
        }

        /// <summary>结束「搜索中…」状态，恢复按钮与下拉。</summary>
        void EndRefresh()
        {
            _searching = false;
            if (_refreshTimer != null) _refreshTimer.Stop();
            _btnRefresh.Text = "刷新";
            _btnRefresh.Enabled = true;
            if (_cboDevice.Items.Count == 0 && _cboDevice.Text == "搜索中…") _cboDevice.Text = "";
            UpdateDeviceHint();
        }

        // ---------------- process control ----------------
        string BuildArgs()
        {
            var sb = new StringBuilder();
            int mi = _cboMonitor.SelectedIndex;
            if (mi > 0) sb.Append("--display ").Append(mi).Append(' ');

            string h = _cboHeight.SelectedIndex == 1 ? "1440" : _cboHeight.SelectedIndex == 2 ? "1080"
                     : _cboHeight.SelectedIndex == 3 ? "720" : "";
            if (h.Length > 0) sb.Append("--height ").Append(h).Append(' ');

            sb.Append("--fps ").Append(_cboFps.SelectedItem).Append(' ');
            sb.Append("--bitrate ").Append(_cboBitrate.SelectedItem).Append(' ');

            int mb = 5;
            switch (_cboLogSize.SelectedIndex)
            {
                case 1: mb = 10; break;
                case 2: mb = 20; break;
                case 3: mb = 50; break;
                case 4: mb = 100; break;
            }
            sb.Append("--log-size ").Append(mb).Append(' ');
            sb.Append("--log \"").Append(EngineLogPath()).Append("\" ");
            if (!_audioOn) sb.Append("--noaudio ");
            if (_switchedNotice) { sb.Append("--switched "); _switchedNotice = false; }

            var dev = _cboDevice.SelectedItem as Discovery.Device;
            if (dev != null)
            {
                // 信标里的 IP/端口来自局域网，必须校验后再拼命令行（旧版不加引号，可被注入额外参数）
                string ip = dev.Ip ?? "";
                System.Net.IPAddress parsed;
                if (System.Net.IPAddress.TryParse(ip, out parsed))
                    sb.Append("--ip \"").Append(ip).Append("\" --port ").Append(dev.Port);
                else
                    Log("忽略非法信标 IP: " + ip);
            }
            return sb.ToString().Trim();
        }

        void StartSender(bool userInitiated = true)
        {
            if (_proc != null && !_proc.HasExited) return;
            if (userInitiated) { _wantCast = true; _retryDelay = 2000; }
            if (!File.Exists(_exePath))
            {
                Log("找不到 XingLianCastSender.exe: " + _exePath);
                MessageBox.Show(this, "找不到 XingLianCastSender.exe：\n" + _exePath + "\n\n请重新安装，或把界面的可执行文件放回安装目录。",
                    "星联投屏", MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }
            if (userInitiated && !License.IsRegistered && Trial.RemainingToday <= 0)
            {
                ShowTrialExhausted();
                return;
            }
            if (_cboDevice.SelectedItem == null)
            {
                SetState("未选择接收端", 3);
                Log("[UI] 尚未发现接收端：确认电视端已启动，等下拉框出现设备后重试");
                return;
            }
            string args = BuildArgs();
            Log("> XingLianCastSender.exe " + args);
            try
            {
                var psi = new ProcessStartInfo(_exePath, args);
                psi.UseShellExecute = false;
                psi.CreateNoWindow = true;
                psi.RedirectStandardOutput = true;
                psi.RedirectStandardError = true;
                psi.StandardOutputEncoding = Encoding.UTF8;
                psi.StandardErrorEncoding = Encoding.UTF8;
                psi.WorkingDirectory = _baseDir;
                _proc = new Process();
                _proc.StartInfo = psi;
                _proc.OutputDataReceived += OnSenderOutput;
                _proc.ErrorDataReceived += OnSenderOutput;
                _proc.EnableRaisingEvents = true;
                _proc.Exited += delegate
                {
                    int exitCode = 0;
                    try { exitCode = _proc.ExitCode; } catch { }
                    BeginInvoke((Action)delegate
                    {
                        // 引擎因未授权/试用用尽退出（exit 3）→ 引导注册，不再自动重试
                        // 电视已被另一台电脑投屏（exit 5）→ 明确提示，不自动重试
                        if (exitCode == 5)
                        {
                            _wantCast = false;
                            StopTrialClock();
                            SetState("电视被占用", 3);
                            ResetMetrics();
                            UpdateButtons();
                            MessageBox.Show(this,
                                "电视正在被另一台电脑投屏（先连的设备优先）。\n\n请等对方停止投屏后再试。",
                                "星联投屏 · 设备忙", MessageBoxButtons.OK, MessageBoxIcon.Information);
                            return;
                        }
                        if (exitCode == 3)
                        {
                            _wantCast = false;
                            StopTrialClock();
                            SetState("未授权", 3);
                            ResetMetrics();
                            UpdateButtons();
                            RefreshLicenseBadge();
                            OpenLicenseDialog();
                            return;
                        }
                        if (exitCode == 7)
                        {
                            // 电视端退出/关机/换 IP：提示后按「自动恢复」设置重连（重连用最新发现的 IP）
                            SetState("电视端已断开，等待重连…", 3);
                            ResetMetrics();
                            UpdateButtons();
                            Log("电视端已断开：接收端 App 退出、电视关机/断网，或 IP 已变化");
                            if (_autoRecover) { ScheduleAutoRecover("电视端断开"); return; }
                            _wantCast = false;
                            StopTrialClock();
                            SetState("电视端已断开", 3);
                            return;
                        }
                        if (exitCode == 6)
                        {
                            // 采集/纹理初始化失败（显卡驱动不支持桌面复制等），不是授权问题
                            _wantCast = false;
                            StopTrialClock();
                            SetState("启动失败", 3);
                            ResetMetrics();
                            UpdateButtons();
                            MessageBox.Show(this,
                                "无法初始化屏幕采集。\n\n" +
                                "常见原因：\n" +
                                "  · 显卡驱动过旧或不支持桌面复制（Desktop Duplication）\n" +
                                "  · 正在远程桌面/虚拟机里运行\n" +
                                "  · 显卡内存不足\n\n" +
                                "建议更新显卡驱动后重试；如仍失败，请把日志发回排查。",
                                "星联投屏 · 采集初始化失败", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                            return;
                        }
                        SetState("已停止", 0);
                        ResetMetrics();
                        UpdateButtons();
                        // 非用户主动停止 → 按自动恢复设置重启
                        if (!_closing && _wantCast) ScheduleAutoRecover("引擎进程退出");
                    });
                };
                _proc.Start();
                _proc.BeginOutputReadLine();
                _proc.BeginErrorReadLine();
                SetState("启动中…", 2);
                if (userInitiated) StartTrialClock();
            }
            catch (Exception ex)
            {
                SetState("启动失败", 3);
                Log("启动失败: " + ex.Message);
                MessageBox.Show(this, "启动投屏引擎失败：\n" + ex.Message, "星联投屏", MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            UpdateButtons();
        }

        void StopSender(bool userInitiated = true)
        {
            if (_proc != null && !_proc.HasExited)
            {
                IntPtr ev = OpenEventW(EVENT_MODIFY_STATE, false, @"Local\XingLianCastStop");
                if (ev != IntPtr.Zero) { SetEvent(ev); CloseHandle(ev); }
                if (!_proc.WaitForExit(1500)) { try { _proc.Kill(); } catch { } }
            }
            StopTrialClock();
            _proc = null;
            _lastTvLost = -1;
            if (userInitiated)
            {
                _wantCast = false;                 // 明确停止：不再自动恢复
                _retryDelay = 2000;
                if (_retryTimer != null) _retryTimer.Stop();
            }
            SetState("已停止", 0);
            ResetMetrics();
            UpdateButtons();
        }

        void OnSenderOutput(object sender, DataReceivedEventArgs e)
        {
            if (e.Data == null) return;
            string line = e.Data;
            if (line.Contains("fps="))
            {
                try
                {
                    BeginInvoke((Action)delegate
                    {
                        SetState("投屏中", 1);
                        UpdateMetrics(line);
                        CheckMonitorAlive();
                    });
                }
                catch { }
                return;
            }
            Log(line);
        }

        static string Field(string line, string key, string suffix)
        {
            int i = line.IndexOf(key);
            if (i < 0) return "-";
            i += key.Length;
            int j = line.IndexOf(' ', i);
            string v = j < 0 ? line.Substring(i) : line.Substring(i, j - i);
            if (suffix.Length > 0 && v.EndsWith(suffix)) v = v.Substring(0, v.Length - suffix.Length);
            return v;
        }
        void ResetMetrics()
        {
            for (int i = 0; i < _mv.Length; i++) if (_mv[i] != null) _mv[i].Set("—");
        }

        /// <summary>把每秒统计行解析到 10 个指标卡片。</summary>
        void UpdateMetrics(string line)
        {
            if (_mv[0] == null) return;
            _mv[0].Set(Field(line, "fps=", "") + " fps");           // 电脑帧率
            _mv[1].Set(Field(line, "编码=", "ms") + " ms");          // 编码耗时/帧
            _mv[2].Set(Field(line, "音流=", "kbps") + " kbps");      // 音频带宽
            _mv[3].Set(Round1(Field(line, "流量=", "Mbps")) + " Mbps"); // 网络流量
            _mv[4].Set(Field(line, "送失=", ""));                    // 电脑丢包/秒（UDP 发送失败，已是每秒增量）
            bool tv = line.Contains("tvfps=");
            _mv[5].Set(tv ? Field(line, "tvfps=", "") + " fps" : "—");   // 电视帧率
            _mv[6].Set(Field(line, "大间隔=", "") + " 次/秒");       // 采集卡顿
            _mv[7].Set(Field(line, "限=", "") + " 帧");              // 限帧丢弃
            _mv[8].Set(tv && !line.Contains("rtt=-1.0") ? Round1(Field(line, "rtt=", "")) + " ms" : "—");
            // 电视丢包/秒：TV 端每秒上报的丢包增量，直接显示
            _mv[9].Set(tv ? Field(line, "tvlost=", "") : "—");
        }

        static string Round1(string s)
        {
            double d;
            if (double.TryParse(s, System.Globalization.NumberStyles.Float,
                                System.Globalization.CultureInfo.InvariantCulture, out d))
                return d.ToString("0.0", System.Globalization.CultureInfo.InvariantCulture);
            return s;
        }
        void UpdateButtons()
        {
            bool running = _proc != null && !_proc.HasExited;
            // 开始按钮保持可用（运行中点击直接 return），这样"投屏中"能保持绿色
            _btnStart.Enabled = true;
            _btnStart.Cursor = running ? Cursors.Default : Cursors.Hand;
            _btnStop.Enabled = running;
        }

        void Log(string line)
        {
            if (string.IsNullOrEmpty(line)) return;
            Debug.WriteLine("[ui] " + line);
        }

        // ---------------- 托盘：原生 Shell_NotifyIcon ----------------
        const int WM_TRAYMSG = 0x0400 + 1;   // WM_APP + 1
        const uint NIM_ADD = 0, NIM_MODIFY = 1, NIM_DELETE = 2;
        const uint NIF_MESSAGE = 0x01, NIF_ICON = 0x02, NIF_TIP = 0x04;

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct NOTIFYICONDATA
        {
            public int cbSize;
            public IntPtr hWnd;
            public uint uID;
            public uint uFlags;
            public uint uCallbackMessage;
            public IntPtr hIcon;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 128)] public string szTip;
            public uint dwState;
            public uint dwStateMask;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)] public string szInfo;
            public uint uTimeoutOrVersion;
            [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)] public string szInfoTitle;
            public uint dwInfoFlags;
            public Guid guidItem;
            public IntPtr hBalloonIcon;
        }

        [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
        static extern bool Shell_NotifyIcon(uint msg, ref NOTIFYICONDATA data);

        Icon _trayIcon;              // 必须常驻，否则其 HICON 会被回收
        bool _trayAdded;

        void SetupTray()
        {
            // 菜单（右键弹出）
            _menu = new ContextMenuStrip();
            _menu.Font = T.F(13);
            _menu.ShowImageMargin = false;
            _menu.Renderer = new ToolStripProfessionalRenderer(new MenuColors());
            _miAutostart = new ToolStripMenuItem("开机自启");
            _miAutostart.CheckOnClick = true;
            _miAutostart.Click += delegate { ApplyAutostart(); UpdateAutostartText(); };
            _menu.Items.Add(_miAutostart);
            _menu.Items.Add(new ToolStripSeparator());

            var miLog = new ToolStripMenuItem("打开日志文件夹");
            miLog.Click += delegate { OpenLogFolder(); };
            _menu.Items.Add(miLog);

            var miLicense = new ToolStripMenuItem("正版注册");
            miLicense.Click += delegate { using (var dlg = new LicenseForm()) dlg.ShowDialog(this); };
            _menu.Items.Add(miLicense);

            var miAbout = new ToolStripMenuItem("关于");
            miAbout.Click += delegate { using (var dlg = new AboutForm()) dlg.ShowDialog(this); };
            _menu.Items.Add(miAbout);

            _menu.Items.Add(new ToolStripSeparator());
            var miQuit = new ToolStripMenuItem("退出");
            miQuit.Click += delegate { _closing = true; Close(); };
            _menu.Items.Add(miQuit);
            LoadAutostartState();
        }

        /// <summary>把图标加进系统托盘（在窗口句柄创建后调用）。返回是否成功。</summary>
        bool TrayAdd()
        {
            if (_trayAdded) return true;
            try
            {
                if (_trayIcon == null)
                {
                    try { _trayIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
                    if (_trayIcon == null) _trayIcon = SystemIcons.Application;
                }
                var d = new NOTIFYICONDATA();
                d.cbSize = Marshal.SizeOf(typeof(NOTIFYICONDATA));
                d.hWnd = Handle;
                d.uID = 1;
                d.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
                d.uCallbackMessage = WM_TRAYMSG;
                d.hIcon = _trayIcon.Handle;
                d.szTip = "星联投屏";
                _trayAdded = Shell_NotifyIcon(NIM_ADD, ref d);
                if (_trayAdded) logx_note("tray icon added");
                else logx_note("tray add failed: 进程完整性=0x" + IntegrityLevel().ToString("X")
                          + "（0x2000=普通, 0x3000=管理员）; 任务栏为 0x3000 时，普通权限进程无法加托盘图标");
                return _trayAdded;
            }
            catch (Exception ex)
            {
                logx_note("tray add exception: " + ex.Message);
                return false;
            }
        }

        [DllImport("advapi32.dll", SetLastError = true)] static extern bool OpenProcessToken(IntPtr p, uint acc, out IntPtr t);
        [DllImport("advapi32.dll", SetLastError = true)] static extern bool GetTokenInformation(IntPtr t, int cls, IntPtr buf, int len, out int need);
        [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();

        /// <summary>当前进程的完整性级别：0x3000=High(管理员) 0x2000=Medium(普通)</summary>
        static int IntegrityLevel()
        {
            IntPtr tok;
            if (!OpenProcessToken(GetCurrentProcess(), 0x0008, out tok)) return -1;
            int need;
            GetTokenInformation(tok, 25, IntPtr.Zero, 0, out need);
            IntPtr buf = Marshal.AllocHGlobal(need);
            int sub = -1;
            if (GetTokenInformation(tok, 25, buf, need, out need))
            {
                IntPtr sid = Marshal.ReadIntPtr(buf);
                byte cnt = Marshal.ReadByte(sid + 1);
                sub = Marshal.ReadInt32(sid + 8 + (cnt - 1) * 4);
            }
            Marshal.FreeHGlobal(buf);
            CloseHandle(tok);
            return sub;
        }

        static bool IsElevated()
        {
            try
            {
                var p = new System.Security.Principal.WindowsPrincipal(
                    System.Security.Principal.WindowsIdentity.GetCurrent());
                return p.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            }
            catch { return false; }
        }

        void logx_note(string s)
        {
            try
            {
                File.AppendAllText(Path.Combine(_baseDir, "ui_tray.log"),
                    DateTime.Now.ToString("HH:mm:ss") + " " + s + Environment.NewLine);
            }
            catch { }
            Debug.WriteLine("[tray] " + s);
        }

        /// <summary>加托盘图标；失败时最多重试 5 次（每次间隔 600ms）。</summary>
        void TryTrayAdd(int attempt)
        {
            if (TrayAdd()) return;
            if (attempt >= 5) { logx_note("tray add gave up（权限级别低于任务栏，或 shell 未就绪）"); return; }
            var t = new System.Windows.Forms.Timer();
            t.Interval = 600;
            t.Tick += delegate
            {
                t.Stop(); t.Dispose();
                TryTrayAdd(attempt + 1);
            };
            t.Start();
        }

        void TrayRemove()
        {
            if (!_trayAdded) return;
            try
            {
                var d = new NOTIFYICONDATA();
                d.cbSize = Marshal.SizeOf(typeof(NOTIFYICONDATA));
                d.hWnd = Handle;
                d.uID = 1;
                Shell_NotifyIcon(NIM_DELETE, ref d);
            }
            catch { }
            _trayAdded = false;
        }

        /// <summary>弹出托盘菜单（右键点迷你条时用）。</summary>
        public void ShowTrayMenu(Point screenPt)
        {
            if (_menu != null) _menu.Show(screenPt);
        }

        public void RestoreFromTray()
        {
            Show();
            if (WindowState == FormWindowState.Minimized) WindowState = FormWindowState.Normal;
            Activate();
            BringToFront();
        }
    }
}
