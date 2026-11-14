using System;
using System.Runtime.InteropServices;
using System.Text;

class P2 {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern IntPtr FindWindowW(string c, string n);
  [DllImport("user32.dll")] static extern IntPtr GetProcessWindowStation();
  [DllImport("user32.dll")] static extern IntPtr GetThreadDesktop(uint tid);
  [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern bool GetUserObjectInformationW(IntPtr h, int i, StringBuilder p, int n, out int need);
  [DllImport("kernel32.dll")] static extern bool ProcessIdToSessionId(uint pid, out uint sid);

  static string Name(IntPtr h) {
    var sb = new StringBuilder(256); int need;
    GetUserObjectInformationW(h, 2, sb, 256, out need);
    return sb.ToString();
  }

  static void Main() {
    uint sid; ProcessIdToSessionId((uint)System.Diagnostics.Process.GetCurrentProcess().Id, out sid);
    Console.WriteLine("session=" + sid);
    Console.WriteLine("winsta=" + Name(GetProcessWindowStation()));
    Console.WriteLine("desktop=" + Name(GetThreadDesktop(GetCurrentThreadId())));
    Console.WriteLine("Shell_TrayWnd=" + FindWindowW("Shell_TrayWnd", null));
    Console.WriteLine("NotifyIconOverflowWindow=" + FindWindowW("NotifyIconOverflowWindow", null));
    Console.WriteLine("Progman=" + FindWindowW("Progman", null));
    Console.WriteLine("IsElevated=" + (new System.Security.Principal.WindowsPrincipal(
        System.Security.Principal.WindowsIdentity.GetCurrent())).IsInRole(
        System.Security.Principal.WindowsBuiltInRole.Administrator));
  }
}
