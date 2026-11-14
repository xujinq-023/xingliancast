using System;
using System.IO;
using System.Security.Cryptography;

class KeyGen2 {
  static void Main(string[] args) {
    try {
      string dir = args.Length > 0 ? args[0] : ".";
      Directory.CreateDirectory(dir);
      var p = new CngKeyCreationParameters();
      p.ExportPolicy = CngExportPolicies.AllowExport;
      using (var key = CngKey.Create(CngAlgorithm.ECDsaP256, null, p)) {
        byte[] priv = key.Export(CngKeyBlobFormat.EccPrivateBlob);
        byte[] pub  = key.Export(CngKeyBlobFormat.EccPublicBlob);
        File.WriteAllBytes(Path.Combine(dir, "license-private.key"), priv);
        string pubB64 = Convert.ToBase64String(pub);
        File.WriteAllText(Path.Combine(dir, "license-public.txt"), pubB64);
        Console.WriteLine("PRIV_LEN=" + priv.Length);
        Console.WriteLine("PUB_LEN=" + pub.Length);
        Console.WriteLine("PUBLIC=" + pubB64);
      }
    } catch (Exception ex) {
      Console.WriteLine("ERR " + ex.GetType().FullName + ": " + ex.Message);
    }
  }
}
