// 星联投屏 · 授权码签发工具（发行方内部使用，内含私钥）
// 用法：
//   MakeLicense.exe /keygen                     生成密钥对（写 license-private.txt / license-public.txt）
//   MakeLicense.exe /machine                    打印本机机器码（自测用）
//   MakeLicense.exe /machine E00C-2577-C37A /days 0     永久授权
//   MakeLicense.exe /machine E00C-2577-C37A /days 365   一年授权
//   /days 0 表示永久；/days N 表示 N 天后到期
using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

static class MakeLicense
{
    static readonly DateTime Epoch = new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    const uint NeverExpire = 0xFFFFFFFFu;

    static int Main(string[] args)
    {
        string dir = AppDomain.CurrentDomain.BaseDirectory;
        string privPath = Path.Combine(dir, "license-private.txt");
        string pubPath = Path.Combine(dir, "license-public.txt");

        string mode = GetArg(args, "/machine");
        bool keygen = Has(args, "/keygen");

        if (keygen)
        {
            using (var ec = new ECDsaCng(256))
            {
                var prm = ec.ExportParameters(true);
                File.WriteAllText(privPath,
                    "curve=nistP256\nX=" + Convert.ToBase64String(prm.Q.X) +
                    "\nY=" + Convert.ToBase64String(prm.Q.Y) +
                    "\nD=" + Convert.ToBase64String(prm.D) + "\n");
                File.WriteAllText(pubPath, Convert.ToBase64String(prm.Q.X) + "|" + Convert.ToBase64String(prm.Q.Y));
                Console.WriteLine("已生成密钥：");
                Console.WriteLine("  私钥 " + privPath + "（不要外传）");
                Console.WriteLine("  公钥 " + pubPath);
                Console.WriteLine("  公钥(源码用) X=" + Convert.ToBase64String(prm.Q.X) + "  Y=" + Convert.ToBase64String(prm.Q.Y));
            }
            return 0;
        }

        if (mode == null)
        {
            PrintUsage();
            return 1;
        }

        if (mode == "auto") mode = LocalMachineCode();
        else
        {
            // 机器码归一：允许无横线/小写，统一成 XXXX-XXXX-XXXX，避免发出"看起来正常"的无效码
            string norm = mode.Replace("-", "").Replace(" ", "").Trim().ToUpperInvariant();
            if (norm.Length != 12)
            {
                Console.WriteLine("错误：机器码应为 12 位十六进制（如 E00C-2577-C37A），实际 \"" + mode + "\"");
                return 2;
            }
            mode = norm.Substring(0, 4) + "-" + norm.Substring(4, 4) + "-" + norm.Substring(8, 4);
        }

        if (!Has(args, "/days") && !Has(args, "/machine")) { PrintUsage(); return 1; }

        if (!Has(args, "/days") && args.Length == 2)
        {
            // 只打印机器码
            Console.WriteLine("机器码: " + mode);
            return 0;
        }

        // /days 必须显式给出且合法：0 = 永久，正整数 = 天数。
        // 旧版 `int.TryParse(ds, out days)` 丢弃结果，笔误（如 "365d"）会被当成 0 → 发成永久授权。
        int days = 0;
        string ds = GetArg(args, "/days");
        if (ds == null)
        {
            Console.WriteLine("错误：缺少 /days。永久请写 /days 0，一年请写 /days 365。");
            return 2;
        }
        if (!int.TryParse(ds.Trim(), out days) || days < 0 || days > 36500)
        {
            Console.WriteLine("错误：/days 的值 \"" + ds + "\" 无效（0 = 永久，或 1~36500 的天数）。");
            return 2;
        }

        string priv = File.ReadAllText(privPath);
        var prm2 = new ECParameters();
        prm2.Curve = ECCurve.NamedCurves.nistP256;
        prm2.Q = new ECPoint
        {
            X = Convert.FromBase64String(GetField(priv, "X=")),
            Y = Convert.FromBase64String(GetField(priv, "Y="))
        };
        prm2.D = Convert.FromBase64String(GetField(priv, "D="));

        var payload = new byte[12];
        payload[0] = 1;
        byte[] mh = MachineHash(mode);
        Array.Copy(mh, 0, payload, 1, 6);
        uint expDays = days <= 0 ? NeverExpire : (uint)Math.Max(0, (DateTime.UtcNow.Date.AddDays(days) - Epoch.Date).Days);
        payload[7] = (byte)(expDays >> 24);
        payload[8] = (byte)(expDays >> 16);
        payload[9] = (byte)(expDays >> 8);
        payload[10] = (byte)expDays;
        payload[11] = 1;   // bit0 = 投屏

        byte[] sig;
        using (var ec = ECDsa.Create())
        {
            ec.ImportParameters(prm2);
            using (var sha = SHA256.Create()) sig = ec.SignHash(sha.ComputeHash(payload));
        }
        // 引擎只接受 r||s 固定 64 字节（license.h: verify 里判 12+64）。若这里输出成 DER，
        // 客户会拿到"看起来正常"却永远注册失败的码，所以必须在签发端拦住。
        if (sig.Length != 64)
        {
            Console.WriteLine("错误：签名长度为 " + sig.Length + " 字节（应为 64）。请检查 .NET 运行时版本。");
            return 3;
        }

        byte[] all = new byte[12 + sig.Length];
        Array.Copy(payload, 0, all, 0, 12);
        Array.Copy(sig, 0, all, 12, sig.Length);
        string code = "XL1." + Convert.ToBase64String(all).Replace('+', '-').Replace('/', '_').TrimEnd('=');

        Console.WriteLine("机器码 : " + mode);
        Console.WriteLine("时长   : " + (days <= 0 ? "永久" : days + " 天"));
        Console.WriteLine("激活码 : " + code);
        Console.WriteLine("长度   : " + code.Length + " 字符");
        try { File.WriteAllText(Path.Combine(dir, "last-license.txt"), code); } catch { }
        return 0;
    }

    static void PrintUsage()
    {
        Console.WriteLine("MakeLicense.exe /keygen");
        Console.WriteLine("MakeLicense.exe /machine                打印本机机器码");
        Console.WriteLine("MakeLicense.exe /machine XXXX-XXXX-XXXX /days 0     永久");
        Console.WriteLine("MakeLicense.exe /machine XXXX-XXXX-XXXX /days 365   一年");
    }

    static bool Has(string[] a, string k) { return GetArg(a, k) != null; }

    static string GetArg(string[] a, string k)
    {
        for (int i = 0; i < a.Length; i++)
            if (string.Equals(a[i], k, StringComparison.OrdinalIgnoreCase))
                return (i + 1 < a.Length) ? a[i + 1] : "auto";
        return null;
    }

    static string GetField(string text, string key)
    {
        foreach (var ln in text.Split('\n'))
            if (ln.TrimStart().StartsWith(key, StringComparison.Ordinal))
                return ln.Substring(ln.IndexOf('=') + 1).Trim();
        return "";
    }

    static byte[] MachineHash(string code)
    {
        using (var sha = SHA256.Create())
            return sha.ComputeHash(Encoding.UTF8.GetBytes(code.Trim().ToUpperInvariant()));
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
