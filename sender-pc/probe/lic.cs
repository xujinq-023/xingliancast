using System;
using System.IO;
using System.Security.Cryptography;
using System.Text;

class LIC {
  const string Secret = "XL-2026-PROJECTOR-SIGNKEY-7f3a91c4";

  static string MachineCode() {
    string vol = "0";
    try {
      var d = new DriveInfo(Path.GetPathRoot(Environment.SystemDirectory));
      if (d != null) vol = d.IsReady ? d.VolumeLabel + ":" + d.TotalSize : "0";
    } catch { }
    string raw = Environment.MachineName + "|" + vol + "|" + Environment.ProcessorCount;
    using (var sha = SHA256.Create()) {
      byte[] h = sha.ComputeHash(Encoding.UTF8.GetBytes(raw));
      var sb = new StringBuilder();
      for (int i = 0; i < 6; i++) sb.Append(h[i].ToString("X2"));
      string hex = sb.ToString();
      return hex.Substring(0,4) + "-" + hex.Substring(4,4) + "-" + hex.Substring(8,4);
    }
  }

  static string ExpectedKey(string mc) {
    using (var sha = SHA256.Create()) {
      byte[] h = sha.ComputeHash(Encoding.UTF8.GetBytes(Secret + "|" + mc));
      var sb = new StringBuilder();
      for (int i = 0; i < 6; i++) sb.Append(h[i].ToString("X2"));
      string hex = sb.ToString();
      return "XL-" + hex.Substring(0,4) + "-" + hex.Substring(4,4) + "-" + hex.Substring(8,4);
    }
  }

  static void Main() {
    string mc = MachineCode();
    Console.WriteLine("machine = " + mc);
    Console.WriteLine("key     = " + ExpectedKey(mc));
  }
}
