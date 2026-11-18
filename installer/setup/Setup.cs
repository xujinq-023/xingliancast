// 星联投屏 安装程序（单文件：内嵌 payload.zip，安装到 %LOCALAPPDATA%\Programs，无需管理员）
// 编译: csc /target:winexe /resource:payload.zip,payload.zip /win32icon:..\payload\logo.ico
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.IO.Compression;
using System.Reflection;
using System.Text;
using System.Runtime.InteropServices;
using System.Security.Cryptography.X509Certificates;
using System.Windows.Forms;
using Microsoft.Win32;

static class RegistryHelper
{
    public static void Write(string dir, string selfExe)
    {
        string uninst = Path.Combine(dir, "uninstall.exe");
        try { File.Copy(selfExe, uninst, true); } catch { }
        using (var k = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + App.AppId))
        {
            k.SetValue("DisplayName", App.Name);
            k.SetValue("DisplayVersion", App.Version);
            k.SetValue("Publisher", App.Publisher);
            k.SetValue("InstallLocation", dir);
            k.SetValue("DisplayIcon", Path.Combine(dir, "logo.ico"));
            k.SetValue("UninstallString", "\"" + uninst + "\" /uninstall");
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
    }
}

static class App
{
    /// <summary>从内嵌 app.ico 加载窗口/任务栏图标。</summary>
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool MoveFileEx(string existing, string newName, int flags);

    public static Icon LoadIcon()
    {
        try
        {
            var s = Assembly.GetExecutingAssembly().GetManifestResourceStream("app.ico");
            if (s != null) return new Icon(s, new Size(256, 256));
        }
        catch { }
        return null;
    }

    public const string Name = "星联投屏";
    public const string Version = "1.0.21";
    public const string Publisher = "星联投屏";
    public const string AppId = "XingLianCast";

    public static bool IsElevated
    {
        get
        {
            try
            {
                var p = new System.Security.Principal.WindowsPrincipal(
                    System.Security.Principal.WindowsIdentity.GetCurrent());
                return p.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator);
            }
            catch { return false; }
        }
    }

    public static string DefaultDir
    {
        get
        {
            // 常规做法：装到 Program Files\XingLianCast（需要管理员）
            // 用户拒绝 UAC 时退回当前用户目录（不需要管理员）
            try
            {
                string pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
                if (!string.IsNullOrEmpty(pf)) return Path.Combine(pf, "XingLianCast");
            }
            catch { }
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                                @"Programs\XingLianCast");
        }
    }

    [STAThread]
    static void Main(string[] args)
    {
        if (args.Length >= 1 && args[0].Equals("--tokeninfo", StringComparison.OrdinalIgnoreCase))
        {
            string line = App.TokenReport();
            try { File.WriteAllText(Path.Combine(Path.GetTempPath(), "xl_token.txt"), line); } catch { }
            try { File.WriteAllText(Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "xl_token.txt"), line); } catch { }
            Environment.Exit(0);
        }

        // 默认按「为所有用户安装」的常规方式：需要管理员权限
        // 用户拒绝 UAC 时继续以当前用户身份安装（目录会自动退回 %LOCALAPPDATA%）
        bool elevatedMarker = Array.Exists(args, a => a.Equals("/elevated", StringComparison.OrdinalIgnoreCase));
        if (!App.IsElevated && !elevatedMarker)
        {
            try
            {
                var quoted = string.Join(" ", Array.ConvertAll(args, a => a.Contains(" ") ? "\"" + a + "\"" : a)).Trim();
                var psi = new ProcessStartInfo(Application.ExecutablePath,
                                               (quoted + " /elevated").Trim());
                psi.UseShellExecute = true;
                psi.Verb = "runas";
                Process.Start(psi);
                return;
            }
            catch { /* 用户点了「否」：继续非提权安装 */ }
        }

        bool uninstall = args.Length > 0 && args[0].ToLowerInvariant().Contains("uninstall");
        bool silent = Array.Exists(args, a => a.Equals("/silent", StringComparison.OrdinalIgnoreCase) ||
                                              a.Equals("/S", StringComparison.OrdinalIgnoreCase));
        if (silent && !uninstall)
        {
            string dir = App.DefaultDir;
            foreach (var a in args)
                if (a.StartsWith("/dir=", StringComparison.OrdinalIgnoreCase)) dir = a.Substring(5).Trim('"');
            var log = new System.Text.StringBuilder();
            try
            {
                string why0;
                string realDir = App.PickWritableDir(dir, out why0);
                if (realDir == null)
                    throw new UnauthorizedAccessException(
                      "无法写入任何候选目录。\n\n进程身份：" + App.TokenReport() +
                      "\n\n各路径失败原因：\n" + why0 +
                      "\n\n如果所有路径都被拒绝（包括临时目录），通常是安全软件拦截或权限令牌受限，\n" +
                      "请把安装包加入杀毒软件白名单，或右键以管理员身份运行。");
                if (!realDir.Equals(dir, StringComparison.OrdinalIgnoreCase))
                    log.AppendLine("fallback dir -> " + realDir);
                dir = realDir;
                App.ExtractPayload(dir);
                log.AppendLine("extract OK -> " + dir);
                log.AppendLine("cert: " + App.InstallCertificate(Path.Combine(dir, "xinglian-codesign.cer"), false));
            }
            catch (Exception ex)
            {
                log.AppendLine("extract FAIL: " + ex.Message);
                File.WriteAllText(Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "setup_error.log"), log.ToString());
                Environment.Exit(2);
            }
            string exe = Path.Combine(dir, "XingLianCast.exe");
            try { App.CreateShortcut(App.StartMenuLnk, exe, dir, Path.Combine(dir, "logo.ico")); log.AppendLine("startmenu OK"); }
            catch (Exception ex) { log.AppendLine("startmenu FAIL: " + ex.Message); }
            try { App.CreateShortcut(App.DesktopLnk, exe, dir, Path.Combine(dir, "logo.ico")); log.AppendLine("desktop OK"); }
            catch (Exception ex) { log.AppendLine("desktop FAIL: " + ex.Message); }
            try { RegistryHelper.Write(dir, Application.ExecutablePath); log.AppendLine("registry OK"); }
            catch (Exception ex) { log.AppendLine("registry FAIL: " + ex.Message); }
            File.WriteAllText(Path.Combine(Path.GetDirectoryName(Application.ExecutablePath), "setup_silent.log"), log.ToString());
            Environment.Exit(0);
        }
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        if (uninstall)
        {
            Application.Run(new UninstallForm());
        }
        else
        {
            Application.Run(new InstallForm());
        }
    }

    // ---------------- 进程令牌诊断 ----------------
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr h, uint acc, out IntPtr tok);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr tok, int cls, IntPtr buf, int len, out int ret);
    [DllImport("kernel32.dll")] static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);

    /// <summary>返回一行诊断：身份 / 是否提权 / 完整性级别 / 是否 AppContainer。</summary>
    public static string TokenReport()
    {
        var sb = new StringBuilder();
        try
        {
            var id = System.Security.Principal.WindowsIdentity.GetCurrent();
            sb.Append("用户=").Append(id.Name);
            var pr = new System.Security.Principal.WindowsPrincipal(id);
            sb.Append("  管理员=").Append(pr.IsInRole(System.Security.Principal.WindowsBuiltInRole.Administrator) ? "是" : "否");
        }
        catch { }
        IntPtr tok = IntPtr.Zero;
        try
        {
            if (OpenProcessToken(GetCurrentProcess(), 0x0008 /*TOKEN_QUERY*/, out tok))
            {
                // TokenIntegrityLevel = 25
                int len = 0;
                GetTokenInformation(tok, 25, IntPtr.Zero, 0, out len);
                if (len > 0)
                {
                    IntPtr buf = Marshal.AllocHGlobal(len);
                    try
                    {
                        int got;
                        if (GetTokenInformation(tok, 25, buf, len, out got))
                        {
                            IntPtr sidPtr = Marshal.ReadIntPtr(buf);
                            var sid = new System.Security.Principal.SecurityIdentifier(sidPtr);
                            int rid = 0;
                            try { var parts = sid.Value.Split('-'); rid = int.Parse(parts[parts.Length - 1]); } catch { }
                            string lvl = rid >= 0x4000 ? "系统" : rid >= 0x3000 ? "高(管理员)" : rid >= 0x2000 ? "中(普通)" : "低/受限";
                            sb.Append("  完整性=").Append(lvl).Append(" (0x").Append(rid.ToString("X")).Append(")");
                        }
                    }
                    finally { Marshal.FreeHGlobal(buf); }
                }
                // TokenIsAppContainer = 29
                int acLen = 0;
                GetTokenInformation(tok, 29, IntPtr.Zero, 0, out acLen);
                if (acLen > 0)
                {
                    IntPtr b2 = Marshal.AllocHGlobal(acLen);
                    try
                    {
                        int got2;
                        if (GetTokenInformation(tok, 29, b2, acLen, out got2) && Marshal.ReadInt32(b2) != 0)
                            sb.Append("  AppContainer=是(沙盒/商店环境)");
                    }
                    finally { Marshal.FreeHGlobal(b2); }
                }
            }
        }
        catch { }
        finally { if (tok != IntPtr.Zero) CloseHandle(tok); }
        return sb.ToString();
    }
    /// <summary>按优先级挑选一个可写目录（默认位置被系统拒绝时自动回退）。</summary>
    public static string PickWritableDir(string preferred) { string ignored; return PickWritableDir(preferred, out ignored); }

    public static string PickWritableDir(string preferred, out string detail)
    {
        var why = new List<string>();
        detail = "";
        var cands = new List<string>();
        cands.Add(preferred);
        string lad0 = "", up0 = "";
        try { lad0 = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData); } catch { }
        try { up0 = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile); } catch { }
        if (lad0.Length > 0) { cands.Add(Path.Combine(lad0, @"Programs\XingLianCast")); cands.Add(Path.Combine(lad0, "XingLianCast")); }
        if (up0.Length > 0)  { cands.Add(Path.Combine(up0, @"Programs\XingLianCast"));  cands.Add(Path.Combine(up0, "XingLianCast")); }
        try { cands.Add(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.CommonDocuments), "XingLianCast")); } catch { }
        try { foreach (var dr in DriveInfo.GetDrives()) { if (dr.DriveType == DriveType.Fixed && !dr.Name.StartsWith("C", StringComparison.OrdinalIgnoreCase)) cands.Add(Path.Combine(dr.Name, "XingLianCast")); } } catch { }
        for (int k = 2; k <= 4; k++) {
            string par = Path.GetDirectoryName(preferred);
            if (!string.IsNullOrEmpty(par)) cands.Add(Path.Combine(par, "XingLianCast-" + k));
        }
        try { cands.Add(Path.Combine(Path.GetTempPath(), "XingLianCast")); } catch { }
        foreach (var d in cands)
        {
            try
            {
                Directory.CreateDirectory(d);
                string probe = Path.Combine(d, ".wtest");
                File.WriteAllText(probe, "x");
                File.Delete(probe);
                return d;
            }
            catch (Exception ex) { why.Add(d + "  →  " + ex.GetType().Name + ": " + ex.Message); }
        }
        detail = string.Join("\n", why.ToArray());
        return null;
    }

    public static void ExtractPayload(string dir)
    {
        Directory.CreateDirectory(dir);
        var asm = Assembly.GetExecutingAssembly();
        using (var s = asm.GetManifestResourceStream("payload.zip"))
        {
            if (s == null) throw new InvalidOperationException("安装包损坏：找不到内嵌文件");
            using (var zip = new ZipArchive(s, ZipArchiveMode.Read))
            {
                foreach (var e in zip.Entries)
                {
                    if (e.FullName.EndsWith("/")) continue;
                    string target = Path.Combine(dir, e.FullName.Replace('/', Path.DirectorySeparatorChar));
                    Directory.CreateDirectory(Path.GetDirectoryName(target));
                    e.ExtractToFile(target, true);
                }
            }
        }
    }

    /// <summary>
    /// 安装自签名代码签名证书，避免用户看到"未知发布者"。
    /// 优先装到「本机」(需管理员，静默)；无权限时退回「当前用户」。
    /// 返回一行结果说明，失败不影响安装。
    /// </summary>
    public static string InstallCertificate(string cerPath) { return InstallCertificate(cerPath, true); }

    /// <summary>interactive=false 时不往 Root 写（避免弹 Windows 确认框阻塞静默安装）。</summary>
    public static string InstallCertificate(string cerPath, bool interactive)
    {
        try
        {
            if (!File.Exists(cerPath)) return "未找到证书文件：" + cerPath;
            var cert = new X509Certificate2(cerPath);

            // 1) 本机存储（管理员）
            if (IsElevated)
            {
                try
                {
                    using (var tp = new X509Store(StoreName.TrustedPublisher, StoreLocation.LocalMachine))
                    {
                        // Root 只在交互安装时写：静默/批量部署往全机根存储塞自签名 CA，
                        // 会永久削弱该机器的信任链（与本方法注释的意图相反）。
                        if (interactive)
                        {
                            using (var root = new X509Store(StoreName.Root, StoreLocation.LocalMachine))
                            {
                                root.Open(OpenFlags.ReadWrite);
                                root.Add(cert);
                            }
                        }
                        tp.Open(OpenFlags.ReadWrite);
                        tp.Add(cert);
                    }
                    return interactive
                        ? "证书已安装到本机受信任存储（对所有用户生效）"
                        : "证书已安装到本机受信任发布者（静默模式不写根存储）";
                }
                catch { }
            }

            // 2) 当前用户存储（无需管理员；Root 可能弹一次确认）
            int ok = 0;
            try
            {
                using (var tp = new X509Store(StoreName.TrustedPublisher, StoreLocation.CurrentUser))
                {
                    tp.Open(OpenFlags.ReadWrite);
                    tp.Add(cert);
                    ok++;
                }
            }
            catch { }
            if (interactive)
            {
                try
                {
                    using (var root = new X509Store(StoreName.Root, StoreLocation.CurrentUser))
                    {
                        root.Open(OpenFlags.ReadWrite);
                        root.Add(cert);
                        ok++;
                    }
                }
                catch { }
            }
            return ok > 0 ? "证书已安装到当前用户存储（仅本账户生效）" : "证书安装失败（可手动双击 .cer 导入）";
        }
        catch (Exception ex)
        {
            return "证书安装失败：" + ex.Message;
        }
    }
    public static string StartMenuLnkPath { get { return StartMenuLnk; } }

    public static void CreateShortcut(string lnkPath, string target, string workDir, string icon)
    {
        object shell = null, sc = null;
        try
        {
            Type t = Type.GetTypeFromProgID("WScript.Shell");
            shell = Activator.CreateInstance(t);
            sc = t.InvokeMember("CreateShortcut", System.Reflection.BindingFlags.InvokeMethod, null, shell, new object[] { lnkPath });
            Type st = sc.GetType();
            st.InvokeMember("TargetPath", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { target });
            st.InvokeMember("WorkingDirectory", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { workDir });
            if (!string.IsNullOrEmpty(icon))
                st.InvokeMember("IconLocation", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { icon });
            st.InvokeMember("Description", System.Reflection.BindingFlags.SetProperty, null, sc, new object[] { Name });
            st.InvokeMember("Save", System.Reflection.BindingFlags.InvokeMethod, null, sc, null);
        }
        finally
        {
            if (sc != null) System.Runtime.InteropServices.Marshal.ReleaseComObject(sc);
            if (shell != null) System.Runtime.InteropServices.Marshal.ReleaseComObject(shell);
        }
    }

    public static string StartMenuLnk
    {
        get
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), Name + ".lnk");
        }
    }

    public static string DesktopLnk
    {
        get
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), Name + ".lnk");
        }
    }
}

#region 主题（浅色卡片 + 蓝色强调）
static class T
{
    public static float S = 1f;
    public static Color Bg = Color.FromArgb(0xF5, 0xF6, 0xF8);
    public static Color Card = Color.White;
    public static Color Text = Color.FromArgb(0x1C, 0x1C, 0x1E);
    public static Color Sub = Color.FromArgb(0x8E, 0x8E, 0x93);
    public static Color Accent = Color.FromArgb(0x3B, 0x82, 0xF6);
    public static Color AccentHi = Color.FromArgb(0x2D, 0x6F, 0xE0);
    public static Color Border = Color.FromArgb(0xE5, 0xE5, 0xEA);

    public static int I(int v) { return (int)Math.Round(v * S); }
    public static Size Sz(int w, int h) { return new Size(I(w), I(h)); }
    public static Point Pt(int x, int y) { return new Point(I(x), I(y)); }
    public static Font F(float size, bool bold = false)
    {
        return new Font("Microsoft YaHei UI", size, bold ? FontStyle.Bold : FontStyle.Regular, GraphicsUnit.Pixel);
    }
    public static void Rounded(Graphics g, Rectangle r, int radius, Color fill, Color border)
    {
        using (var path = new GraphicsPath())
        {
            int d = radius * 2;
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            using (var b = new SolidBrush(fill)) g.FillPath(b, path);
            if (border.A > 0)
                using (var p = new Pen(border)) g.DrawPath(p, path);
        }
    }
}
#endregion

class FlatButton : Button
{
    public Color Fill = T.Accent, FillHover = T.AccentHi;
    bool _hover;
    public FlatButton()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint |
                 ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
        FlatStyle = FlatStyle.Flat;
        FlatAppearance.BorderSize = 0;
        ForeColor = Color.White;
        Font = T.F(13.5f);
        Cursor = Cursors.Hand;
        BackColor = T.Bg;          // 与窗体底色一致，圆角外不会被刷成黑色
    }
    protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        // AllPaintingInWmPaint 下不再单独刷背景：圆角外四个角必须自己填，否则露出黑边
        Color bgc = BackColor;
        if (bgc.A == 0 || bgc == Color.Transparent) bgc = (Parent != null ? Parent.BackColor : T.Bg);
        using (var bg = new SolidBrush(bgc)) g.FillRectangle(bg, ClientRectangle);
        g.SmoothingMode = SmoothingMode.AntiAlias;
        T.Rounded(g, new Rectangle(0, 0, Width - 1, Height - 1), T.I(8), _hover ? FillHover : Fill, Color.Transparent);
        TextRenderer.DrawText(g, Text, Font, ClientRectangle, ForeColor,
            TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
    }
}

class InstallForm : Form
{
    readonly TextBox _dir;
    readonly CheckBox _desktop;
    readonly CheckBox _run;
    readonly ProgressBar _bar;
    readonly Label _status;
    readonly FlatButton _btnInstall, _btnCancel;
    readonly List<string> _warn = new List<string>();
    bool _installing;

    public InstallForm()
    {
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) T.S = g.DpiX / 96f;
        Text = App.Name + " 安装程序";
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = T.Sz(520, 352);
        BackColor = T.Bg;
        Font = T.F(12.5f);
        AutoScaleMode = AutoScaleMode.None;
        var ico = App.LoadIcon();
        if (ico != null) Icon = ico;

        var title = new Label();
        title.Text = "安装 " + App.Name;
        title.Font = T.F(20, true);
        title.ForeColor = T.Text;
        title.Location = T.Pt(28, 22);
        title.Size = T.Sz(400, 34);
        Controls.Add(title);

        var sub = new Label();
        sub.Text = "版本 " + App.Version + "   ·   Windows → 智慧屏 局域网投屏";
        sub.ForeColor = T.Sub;
        sub.Location = T.Pt(30, 60);
        sub.Size = T.Sz(440, 20);
        Controls.Add(sub);

        var capDir = new Label();
        capDir.Text = "安装位置";
        capDir.ForeColor = T.Sub;
        capDir.Location = T.Pt(30, 104);
        capDir.Size = T.Sz(80, 20);
        Controls.Add(capDir);

        _dir = new TextBox();
        _dir.Text = App.DefaultDir;
        _dir.Location = T.Pt(30, 126);
        _dir.Size = T.Sz(360, 26);
        _dir.BorderStyle = BorderStyle.FixedSingle;
        Controls.Add(_dir);

        var browse = new FlatButton();
        browse.Text = "浏览…";
        browse.Fill = Color.FromArgb(0xEF, 0xF1, 0xF5);
        browse.FillHover = T.Border;
        browse.ForeColor = T.Text;
        browse.Location = T.Pt(400, 124);
        browse.Size = T.Sz(88, 30);
        browse.Click += delegate
        {
            using (var d = new FolderBrowserDialog())
            {
                d.Description = "选择安装位置";
                d.SelectedPath = _dir.Text;
                if (d.ShowDialog(this) == DialogResult.OK) _dir.Text = Path.Combine(d.SelectedPath, "XingLianCast");
            }
        };
        Controls.Add(browse);

        _desktop = new CheckBox();
        _desktop.Text = "创建桌面快捷方式";
        _desktop.Checked = true;
        _desktop.ForeColor = T.Text;
        _desktop.Location = T.Pt(30, 164);
        _desktop.Size = T.Sz(220, 24);
        Controls.Add(_desktop);

        _run = new CheckBox();
        _run.Text = "安装完成后立即启动";
        _run.Checked = true;
        _run.ForeColor = T.Text;
        _run.Location = T.Pt(30, 190);
        _run.Size = T.Sz(220, 24);
        Controls.Add(_run);

        _bar = new ProgressBar();
        _bar.Location = T.Pt(30, 222);
        _bar.Size = T.Sz(456, 8);
        _bar.Style = ProgressBarStyle.Continuous;
        _bar.Visible = false;
        Controls.Add(_bar);

        _status = new Label();
        _status.Text = "首次运行会在系统防火墙弹窗中请求网络权限，请选择「允许访问」。";
        _status.ForeColor = T.Sub;
        _status.Location = T.Pt(30, 236);
        _status.Size = T.Sz(456, 44);
        _status.Font = T.F(11.5f);
        Controls.Add(_status);

        _btnInstall = new FlatButton();
        _btnInstall.Text = "安装";
        _btnInstall.Location = T.Pt(292, 296);
        _btnInstall.Size = T.Sz(96, 34);
        _btnInstall.Click += delegate { StartInstall(); };
        Controls.Add(_btnInstall);

        _btnCancel = new FlatButton();
        _btnCancel.Text = "取消";
        _btnCancel.Fill = Color.FromArgb(0xEF, 0xF1, 0xF5);
        _btnCancel.FillHover = T.Border;
        _btnCancel.ForeColor = T.Text;
        _btnCancel.Location = T.Pt(396, 296);
        _btnCancel.Size = T.Sz(90, 34);
        _btnCancel.Click += delegate { if (!_installing) Close(); };
        Controls.Add(_btnCancel);
    }

    void StartInstall()
    {
        string dir = _dir.Text.Trim();
        if (dir.Length == 0) { MessageBox.Show(this, "请填写安装位置", App.Name); return; }
        if (Directory.Exists(dir) && File.Exists(Path.Combine(dir, "XingLianCast.exe")))
        {
            if (MessageBox.Show(this, "该目录已安装星联投屏，是否覆盖安装？", App.Name,
                    MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes) return;
        }
        _installing = true;
        _btnInstall.Enabled = false;
        _btnCancel.Enabled = false;
        _desktop.Enabled = false;
        _run.Enabled = false;
        _dir.Enabled = false;
        _bar.Visible = true;
        _bar.Value = 5;
        _status.Text = "正在解压文件…";

        var t = new System.Threading.Thread((System.Threading.ThreadStart)delegate
        {
            string err = null;
            try
            {
                string realDir = App.PickWritableDir(dir);
                string why;
                string realDir2 = App.PickWritableDir(dir, out why);
                realDir = realDir2;
                if (realDir == null)
                    throw new UnauthorizedAccessException("无法写入任何候选目录。\n\n进程身份：" + App.TokenReport() + "\n\n各路径失败原因：\n" + why);
                if (!realDir.Equals(dir, StringComparison.OrdinalIgnoreCase))
                    BeginInvoke((Action)delegate { _status.Text = "默认位置不可写，改用：" + realDir; });
                dir = realDir;
                App.ExtractPayload(dir);
                BeginInvoke((Action)delegate { _bar.Value = 62; _status.Text = "正在安装代码签名证书…"; });
                string certMsg = App.InstallCertificate(Path.Combine(dir, "xinglian-codesign.cer"));
                if (certMsg.StartsWith("证书安装失败")) _warn.Add(certMsg);
                BeginInvoke((Action)delegate { _bar.Value = 70; _status.Text = "正在创建快捷方式…"; });
                string exe = Path.Combine(dir, "XingLianCast.exe");
                try { App.CreateShortcut(App.StartMenuLnk, exe, dir, Path.Combine(dir, "logo.ico")); }
                catch (Exception e1) { _warn.Add("开始菜单快捷方式：" + e1.Message); }
                if (_desktop.Checked)
                {
                    try { App.CreateShortcut(App.DesktopLnk, exe, dir, Path.Combine(dir, "logo.ico")); }
                    catch (Exception e2) { _warn.Add("桌面快捷方式：" + e2.Message); }
                }
                BeginInvoke((Action)delegate { _bar.Value = 85; _status.Text = "正在注册卸载信息…"; });
                try { RegistryHelper.Write(dir, Application.ExecutablePath); }
                catch (Exception e3) { _warn.Add("卸载注册信息：" + e3.Message); }
            }
            catch (Exception ex) { err = ex.Message; }

            BeginInvoke((Action)delegate
            {
                if (err != null)
                {
                    _bar.Visible = false;
                    _status.Text = "安装失败（已弹出详细信息）。可换一个安装位置后重试。";
                    _status.ForeColor = Color.FromArgb(0xD0, 0x3A, 0x3A);
                    MessageBox.Show(this,
                        "安装失败：\n\n" + err + "\n\n可能原因与处理：\n" +
                        "· 目标目录被占用或权限不足 → 换一个安装位置（例如 D:\\星联投屏）\n" +
                        "· 杀毒软件「受控文件夹访问」拦截 → 临时关闭后重试\n" +
                        "· 该目录已存在且属于其他用户 → 右键以管理员身份运行本安装包",
                        App.Name, MessageBoxButtons.OK, MessageBoxIcon.Error);
                    _btnInstall.Enabled = true;
                    _btnCancel.Enabled = true;
                    _installing = false;
                    return;
                }
                _bar.Value = 100;
                _status.Text = "安装完成。";
                FinishInstall(dir);
            });
        });
        t.IsBackground = true;
        t.Start();
    }

    void WriteUninstallEntry(string dir)
    {
        string uninst = Path.Combine(dir, "uninstall.exe");
        string self = Application.ExecutablePath;
        try { File.Copy(self, uninst, true); } catch { }

        using (var k = Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + App.AppId))
        {
            k.SetValue("DisplayName", App.Name);
            k.SetValue("DisplayVersion", App.Version);
            k.SetValue("Publisher", App.Publisher);
            k.SetValue("InstallLocation", dir);
            k.SetValue("DisplayIcon", Path.Combine(dir, "logo.ico"));
            k.SetValue("UninstallString", "\"" + uninst + "\" /uninstall");
            k.SetValue("NoModify", 1, RegistryValueKind.DWord);
            k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        }
    }

    void FinishInstall(string dir)
    {
        _installing = false;
        if (_run.Checked)
        {
            try
            {
                Process.Start(new ProcessStartInfo(Path.Combine(dir, "XingLianCast.exe")) { WorkingDirectory = dir });
            }
            catch { }
        }
        if (_warn.Count > 0)
        {
            MessageBox.Show(this, "已安装，但以下步骤未完成（不影响使用）：\n\n· " + string.Join("\n· ", _warn.ToArray()),
                App.Name, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
        MessageBox.Show(this, "星联投屏 已安装完成。\n\n" +
            "· 桌面/开始菜单中可找到「星联投屏」\n" +
            "· 手机或电视端请安装同一目录下的 receiver-tv.apk\n" +
            "· 首次运行如出现防火墙提示，请选择「允许访问」",
            App.Name, MessageBoxButtons.OK, MessageBoxIcon.Information);
        Close();
    }
}

class UninstallForm : Form
{
    public UninstallForm()
    {
        using (var g = Graphics.FromHwnd(IntPtr.Zero)) T.S = g.DpiX / 96f;
        Text = App.Name + " 卸载";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = T.Sz(420, 210);
        BackColor = T.Bg;
        Font = T.F(12.5f);
        AutoScaleMode = AutoScaleMode.None;
        MaximizeBox = false;
        var icoU = App.LoadIcon();
        if (icoU != null) Icon = icoU;

        string dir = FindInstallDir();

        var title = new Label();
        title.Text = "卸载 " + App.Name;
        title.Font = T.F(18, true);
        title.ForeColor = T.Text;
        title.Location = T.Pt(26, 20);
        title.Size = T.Sz(300, 30);
        Controls.Add(title);

        var info = new Label();
        info.Text = "安装位置：" + dir + "\n\n将删除程序文件、快捷方式和卸载注册信息。\n接收端 APK 与日志目录会被保留。";
        info.ForeColor = T.Sub;
        info.Location = T.Pt(28, 62);
        info.Size = T.Sz(370, 80);
        Controls.Add(info);

        var ok = new FlatButton();
        ok.Text = "卸载";
        ok.Fill = Color.FromArgb(0xD0, 0x3A, 0x3A);
        ok.FillHover = Color.FromArgb(0xB5, 0x30, 0x30);
        ok.Location = T.Pt(216, 158);
        ok.Size = T.Sz(90, 34);
        ok.Click += delegate { DoUninstall(dir); };
        Controls.Add(ok);

        var cancel = new FlatButton();
        cancel.Text = "取消";
        cancel.Fill = Color.FromArgb(0xEF, 0xF1, 0xF5);
        cancel.FillHover = T.Border;
        cancel.ForeColor = T.Text;
        cancel.Location = T.Pt(316, 158);
        cancel.Size = T.Sz(90, 34);
        cancel.Click += delegate { Close(); };
        Controls.Add(cancel);
    }

    static string FindInstallDir()
    {
        try
        {
            using (var k = Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + App.AppId))
            {
                if (k != null)
                {
                    object v = k.GetValue("InstallLocation");
                    if (v != null) return v.ToString();
                }
            }
        }
        catch { }
        return App.DefaultDir;
    }

    void DoUninstall(string dir)
    {
        try
        {
            foreach (var p in Process.GetProcessesByName("XingLianCast")) { try { p.Kill(); } catch { } }
            foreach (var p in Process.GetProcessesByName("XingLianCastSender")) { try { p.Kill(); } catch { } }
            System.Threading.Thread.Sleep(400);

            TryDelete(App.StartMenuLnk);
            TryDelete(App.DesktopLnk);
            // 开机自启快捷方式（autostart.ps1 创建）与授权/试用备份残留：旧版都漏了
            try {
                string startup = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.Startup), App.Name + ".lnk");
                TryDelete(startup);
            } catch { }
            try { Registry.CurrentUser.DeleteSubKeyTree(@"Software\XingLian\Projector", false); } catch { }
            try { Registry.CurrentUser.DeleteSubKeyTree(@"Software\Microsoft\Windows\CurrentVersion\Uninstall\" + App.AppId, false); } catch { }

            // 保留 APK 和日志，便于重装后继续用
            string keep1 = Path.Combine(dir, "receiver-tv.apk");
            string apkBackup = Path.Combine(Path.GetTempPath(), "xl_apk.tmp");
            bool hasApk = File.Exists(keep1);
            if (hasApk) { try { File.Copy(keep1, apkBackup, true); } catch { hasApk = false; } }

            string logs = Path.Combine(dir, "logs");
            string logsBackup = Path.Combine(Path.GetTempPath(), "xl_logs");
            bool hasLogs = Directory.Exists(logs);
            if (hasLogs)
            {
                try
                {
                    if (Directory.Exists(logsBackup)) Directory.Delete(logsBackup, true);
                    Directory.Move(logs, logsBackup);
                }
                catch { hasLogs = false; }
            }

            // 删除程序文件：占用中的文件删不掉时重试，仍不行就登记「重启后删除」
            int stuck = 0;
            for (int attempt = 0; attempt < 3 && Directory.Exists(dir); attempt++)
            {
                try { Directory.Delete(dir, true); break; }
                catch
                {
                    try
                    {
                        foreach (var f2 in Directory.GetFiles(dir, "*", SearchOption.AllDirectories))
                        {
                            try { File.Delete(f2); }
                            catch { stuck++; App.MoveFileEx(f2, null, 4); }   // MOVEFILE_DELAY_UNTIL_REBOOT
                        }
                        foreach (var d2 in Directory.GetDirectories(dir, "*", SearchOption.AllDirectories))
                        { try { Directory.Delete(d2, true); } catch { } }
                    }
                    catch { }
                    System.Threading.Thread.Sleep(350);
                }
            }
            if (Directory.Exists(dir)) { try { App.MoveFileEx(dir, null, 4); stuck++; } catch { } }

            if (hasApk) { try { File.Copy(apkBackup, keep1, true); } catch { } }

            Application.DoEvents();
            if ((hasApk || hasLogs) && !Directory.Exists(dir)) Directory.CreateDirectory(dir);
            if (hasApk) { try { File.Copy(apkBackup, keep1, true); } catch { } }
            if (hasLogs) { try { if (!Directory.Exists(logs)) Directory.Move(logsBackup, logs); } catch { } }

            MessageBox.Show(stuck > 0
                    ? "卸载完成，但有文件正被占用，已登记为重启后自动删除。\n请重启电脑完成清理。\n\n保留内容：" + (hasApk ? "\n· receiver-tv.apk" : "") + (hasLogs ? "\n· logs 日志目录" : "")
                    : "卸载完成。\n\n保留内容：" + (hasApk ? "\n· receiver-tv.apk" : "") + (hasLogs ? "\n· logs 日志目录" : ""),
                App.Name, MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (Exception ex)
        {
            MessageBox.Show("卸载时出现问题：" + ex.Message, App.Name, MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }

        // 自删除：交给 cmd 处理，避免文件占用
        try
        {
            string self = Application.ExecutablePath;
            var psi = new ProcessStartInfo("cmd.exe",
                "/c ping -n 3 127.0.0.1 >nul & del /f /q \"" + self + "\"");
            psi.CreateNoWindow = true;
            psi.UseShellExecute = false;
            Process.Start(psi);
        }
        catch { }
        Close();
    }

    static void TryDelete(string path)
    {
        try { if (File.Exists(path)) File.Delete(path); } catch { }
    }
}
