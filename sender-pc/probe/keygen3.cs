using System;
using System.IO;
using System.Security.Cryptography;

class KeyGen3 {
  static void Main(string[] args) {
    try {
      string dir = args.Length > 0 ? args[0] : ".";
      Directory.CreateDirectory(dir);
      using (var ec = new ECDsaCng(256)) {
        var prm = ec.ExportParameters(true);
        string xml = "curve=nistP256\nX=" + Convert.ToBase64String(prm.Q.X) +
                     "\nY=" + Convert.ToBase64String(prm.Q.Y) +
                     "\nD=" + Convert.ToBase64String(prm.D) + "\n";
        File.WriteAllText(Path.Combine(dir, "license-private.txt"), xml);
        string pub = Convert.ToBase64String(prm.Q.X) + "|" + Convert.ToBase64String(prm.Q.Y);
        File.WriteAllText(Path.Combine(dir, "license-public.txt"), pub);
        Console.WriteLine("PUB=" + pub);
        // 自检：签一段再验
        byte[] data = System.Text.Encoding.UTF8.GetBytes("selftest");
        byte[] hash = SHA256.Create().ComputeHash(data);
        byte[] sig = ec.SignHash(hash);
        Console.WriteLine("SIGLEN=" + sig.Length + " VERIFY=" + ec.VerifyHash(hash, sig));
      }
    } catch (Exception ex) {
      Console.WriteLine("ERR " + ex.GetType().FullName + ": " + ex.Message);
    }
  }
}
