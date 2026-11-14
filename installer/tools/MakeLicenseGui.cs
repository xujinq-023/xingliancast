// 星联投屏 · 激活码生成器（图形界面，发行方内部使用）
// 需与 license-private.txt 放在同一目录
using System;
using System.Drawing;
using System.IO;
using System.Security.Cryptography;
using System.Text;
using System.Windows.Forms;

static class Program
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);
        Application.Run(new GenForm());
    }
}

class GenForm : Form
{
    static readonly DateTime Epoch = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    const uint NeverExpire = 0xFFFFFFFFu;

    readonly TextBox _machine = new TextBox();
    readonly ComboBox _duration = new ComboBox();
    readonly TextBox _days = new TextBox();
    readonly TextBox _out = new TextBox();
    readonly Label _status = new Label();

    public GenForm()
    {
        Text = "星联投屏 · 激活码生成器";
        ClientSize = new Size(560, 400);
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Microsoft YaHei UI", 10f);
        BackColor = Color.FromArgb(0xF5, 0xF6, 0xF8);

        var t = new Label
        {
            Text = "激活码生成器", Font = new Font("Microsoft YaHei UI", 17f, FontStyle.Bold),
            ForeColor = Color.FromArgb(0x1C, 0x1C, 0x1E), Location = new Point(22, 16), AutoSize = true
        };
        Controls.Add(t);

        var sub = new Label
        {
            Text = "把客户发来的机器码粘进来，选时长，点生成，复制激活码发给客户",
            ForeColor = Color.FromArgb(0x8E, 0x8E, 0x93), Location = new Point(24, 52), AutoSize = true
        };
        Controls.Add(sub);

        // 机器码
        Controls.Add(new Label { Text = "客户机器码", Location = new Point(24, 88), AutoSize = true, ForeColor = Color.FromArgb(0x8E, 0x8E, 0x93) });
        _machine.Location = new Point(24, 110);
        _machine.HideSelection = false;
        _machine.Size = new Size(300, 28);
        Controls.Add(_machine);
        _machine.GotFocus += delegate { _machine.SelectAll(); };

        var paste = new Button { Text = "粘贴", Location = new Point(336, 109), Size = new Size(76, 30) };
        paste.Click += delegate { try { _machine.Text = Clipboard.GetText().Trim(); } catch { } };
        Controls.Add(paste);

        // 本机按钮
        var mine = new Button { Text = "用本机", Location = new Point(420, 109), Size = new Size(84, 30) };
        mine.Click += delegate { _machine.Text = LocalMachineCode(); };
        Controls.Add(mine);

        // 时长
        Controls.Add(new Label { Text = "授权时长", Location = new Point(24, 152), AutoSize = true, ForeColor = Color.FromArgb(0x8E, 0x8E, 0x93) });
        _duration.Location = new Point(24, 174);
        _duration.Size = new Size(160, 28);
        _duration.DropDownStyle = ComboBoxStyle.DropDownList;
        _duration.Items.AddRange(new object[] { "永久", "365 天", "180 天", "90 天", "30 天", "自定义天数" });
        _duration.SelectedIndex = 0;
        Controls.Add(_duration);

        _days.Location = new Point(196, 174);
        _days.Size = new Size(80, 28);
        _days.Enabled = false;
        _days.Text = "30";
        Controls.Add(_days);
        _duration.SelectedIndexChanged += delegate
        {
            _days.Enabled = _duration.SelectedIndex == 5;
        };

        var gen = new Button
        {
            Text = "生成激活码", Location = new Point(292, 172), Size = new Size(212, 34),
            BackColor = Color.FromArgb(0x3B, 0x82, 0xF6), ForeColor = Color.White, FlatStyle = FlatStyle.Flat
        };
        gen.FlatAppearance.BorderSize = 0;
        gen.Click += delegate { Generate(); };
        Controls.Add(gen);

        // 结果
        var outCap = new Label { Text = "激活码（点一下即可复制）", Location = new Point(24, 220), AutoSize = true, ForeColor = Color.FromArgb(0x8E, 0x8E, 0x93) };
        Controls.Add(outCap);
        _out.Location = new Point(24, 242);
        _out.Size = new Size(512, 96);
        _out.Multiline = true;
        _out.ReadOnly = true;
        _out.ScrollBars = ScrollBars.Vertical;
        _out.Font = new Font("Consolas", 8.5f);
        _out.BackColor = Color.White;
        Controls.Add(_out);

        // 点一下激活码框就复制（最常用操作）
        _out.Cursor = Cursors.Hand;
        _out.Click += delegate
        {
            if (_out.Text.Length == 0) { _status.Text = "还没有生成激活码"; return; }
            try
            {
                Clipboard.SetText(_out.Text);
                _status.ForeColor = Color.FromArgb(0x2E, 0xA5, 0x62);
                _status.Text = "已复制到剪贴板（" + _out.Text.Length + " 字符），直接粘贴给客户";
            }
            catch { _status.Text = "复制失败，请手动全选复制"; }
        };

        var copy = new Button { Text = "复制激活码", Location = new Point(24, 348), Size = new Size(120, 34) };
        copy.Click += delegate
        {
            if (_out.Text.Length == 0) { _status.Text = "还没有生成激活码"; return; }
            try { Clipboard.SetText(_out.Text); _status.Text = "已复制到剪贴板，直接粘贴给客户即可"; }
            catch { _status.Text = "复制失败，请手动全选复制"; }
        };
        Controls.Add(copy);

        _status.Location = new Point(156, 356);
        _status.Size = new Size(380, 22);
        _status.ForeColor = Color.FromArgb(0x2E, 0xA5, 0x62);
        Controls.Add(_status);
    }

    void Generate()
    {
        string mc = _machine.Text.Trim().ToUpperInvariant();
        if (mc.Length == 0) { _status.Text = "请先填写客户的机器码"; _status.ForeColor = Color.FromArgb(0xD0, 0x3A, 0x3A); return; }
        // 机器码格式：XXXX-XXXX-XXXX（大小写不敏感，允许无横线）
        string norm = mc.Replace("-", "");
        if (norm.Length != 12) { _status.Text = "机器码应为 12 位（如 E00C-2577-C37A）"; _status.ForeColor = Color.FromArgb(0xD0, 0x3A, 0x3A); return; }
        mc = norm.Substring(0, 4) + "-" + norm.Substring(4, 4) + "-" + norm.Substring(8, 4);

        int days;
        switch (_duration.SelectedIndex)
        {
            case 0: days = 0; break;
            case 1: days = 365; break;
            case 2: days = 180; break;
            case 3: days = 90; break;
            case 4: days = 30; break;
            default:
                if (!int.TryParse(_days.Text, out days) || days <= 0)
                {
                    _status.Text = "自定义天数请填正整数"; _status.ForeColor = Color.FromArgb(0xD0, 0x3A, 0x3A); return;
                }
                break;
        }

        try
        {
            string code = Sign(mc, days);
            _out.Text = code;
            _status.ForeColor = Color.FromArgb(0x2E, 0xA5, 0x62);
            _status.Text = "已生成：" + mc + "，" + (days <= 0 ? "永久" : days + " 天") + "（" + code.Length + " 字符）";
        }
        catch (Exception ex)
        {
            _status.ForeColor = Color.FromArgb(0xD0, 0x3A, 0x3A);
            _status.Text = "生成失败：" + ex.Message;
        }
    }

    string Sign(string machineCode, int days)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        string privPath = Path.Combine(dir, "license-private.txt");
        if (!File.Exists(privPath)) throw new Exception("找不到 license-private.txt（需与本程序同目录）");
        string priv = File.ReadAllText(privPath);

        var prm = new ECParameters();
        prm.Curve = ECCurve.NamedCurves.nistP256;
        prm.Q = new ECPoint
        {
            X = Convert.FromBase64String(GetField(priv, "X=")),
            Y = Convert.FromBase64String(GetField(priv, "Y="))
        };
        prm.D = Convert.FromBase64String(GetField(priv, "D="));

        var payload = new byte[12];
        payload[0] = 1;
        using (var sha = SHA256.Create())
        {
            byte[] mh = sha.ComputeHash(Encoding.UTF8.GetBytes(machineCode));
            Array.Copy(mh, 0, payload, 1, 6);
        }
        uint expDays = days <= 0 ? NeverExpire
            : (uint)Math.Max(0, (DateTime.UtcNow.Date.AddDays(days) - Epoch.Date).Days);
        payload[7] = (byte)(expDays >> 24);
        payload[8] = (byte)(expDays >> 16);
        payload[9] = (byte)(expDays >> 8);
        payload[10] = (byte)expDays;
        payload[11] = 1;

        byte[] sig;
        using (var ec = ECDsa.Create())
        {
            ec.ImportParameters(prm);
            using (var sha = SHA256.Create()) sig = ec.SignHash(sha.ComputeHash(payload));
        }
        byte[] all = new byte[12 + sig.Length];
        Array.Copy(payload, 0, all, 0, 12);
        Array.Copy(sig, 0, all, 12, sig.Length);
        return "XL1." + Convert.ToBase64String(all).Replace('+', '-').Replace('/', '_').TrimEnd('=');
    }

    static string GetField(string text, string key)
    {
        foreach (var ln in text.Split('\n'))
            if (ln.TrimStart().StartsWith(key, StringComparison.Ordinal))
                return ln.Substring(ln.IndexOf('=') + 1).Trim();
        return "";
    }

    static string LocalMachineCode()
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
}
