using System;
using System.IO;
using System.Security.Cryptography;

class KeyGen {
  static void Main(string[] args) {
    if (args.Length < 1) { Console.WriteLine("usage: keygen <outdir>"); return; }
    string dir = args[0];
    Directory.CreateDirectory(dir);
    var p = new CngKeyCreationParameters();
    p.ExportPolicy = CngExportPolicies.AllowExport;
    using (var key = CngKey.Create(CngAlgorithm.ECDsaP256, null, p)) {
      byte[] priv = key.Export(CngKeyBlobFormat.EccPrivateBlob);
      byte[] pub  = key.Export(CngKeyBlobFormat.EccPublicBlob);
      File.WriteAllBytes(Path.Combine(dir, "license-private.key"), priv);
      File.WriteAllText(Path.Combine(dir, "license-public.txt"), Convert.ToBase64String(pub));
      Console.WriteLine("PRIVATE: " + Path.Combine(dir, "license-private.key") + " (" + priv.Length + " bytes)");
      Console.WriteLine("PUBLIC : " + Convert.ToBase64String(pub));
    }
  }
}
