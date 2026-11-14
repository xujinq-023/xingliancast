using System;
using System.Diagnostics;
using System.Text;
class T {
  static void Main() {
    var psi = new ProcessStartInfo("netsh", "advfirewall firewall show rule name=\"Projector Discovery\"");
    psi.UseShellExecute = false; psi.CreateNoWindow = true;
    psi.RedirectStandardOutput = true; psi.RedirectStandardError = true;
    psi.StandardOutputEncoding = Encoding.UTF8;
    var p = Process.Start(psi);
    string o = p.StandardOutput.ReadToEnd();
    p.WaitForExit(5000);
    Console.WriteLine("exit=" + p.ExitCode);
    Console.WriteLine("found=" + (o.IndexOf("Projector Discovery", StringComparison.OrdinalIgnoreCase) >= 0));
    Console.WriteLine("len=" + o.Length);
    Console.WriteLine(o.Substring(0, Math.Min(200, o.Length)));
  }
}
