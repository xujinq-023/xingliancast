using System;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
class T {
  [DllImport("kernel32.dll", SetLastError = true)]
  static extern bool SetHandleInformation(IntPtr h, uint mask, uint flags);
  static void Main(string[] a) {
    var udp = new UdpClient(45691);
    if (a.Length > 0 && a[0] == "fix") {
      SetHandleInformation(udp.Client.Handle, 0x1, 0);   // HANDLE_FLAG_INHERIT = 0
      Console.WriteLine("socket handle marked non-inheritable");
    } else {
      Console.WriteLine("socket handle left inheritable (default)");
    }
    // 起一个子进程，模拟界面启动 sender（这里用 timeout 代替，跑 6 秒）
    var p = Process.Start(new ProcessStartInfo("cmd", "/c timeout /t 6 >nul")
    { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true });
    p.BeginOutputReadLine();
    Console.WriteLine("child pid=" + p.Id);
    Console.WriteLine("parent exiting now (socket handle inherited?)");
  }
}
