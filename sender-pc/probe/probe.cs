using System;
using System.Runtime.InteropServices;

class P {
  [DllImport("kernel32.dll")] static extern IntPtr GetModuleHandle(string n);
  [DllImport("user32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern IntPtr CreateWindowExW(int ex, string cls, string name, int style,
      int x, int y, int w, int h, IntPtr parent, IntPtr menu, IntPtr inst, IntPtr param);
  [DllImport("user32.dll")] static extern IntPtr FindWindowW(string cls, string name);
  [DllImport("user32.dll")] static extern bool IsWindow(IntPtr h);
  [DllImport("shell32.dll", CharSet=CharSet.Unicode, SetLastError=true)]
  static extern bool Shell_NotifyIconW(uint msg, ref NID data);
  [DllImport("user32.dll")] static extern IntPtr LoadIconW(IntPtr inst, IntPtr name);

  [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
  struct NID {
    public int cbSize; public IntPtr hWnd; public uint uID; public uint uFlags;
    public uint uCallbackMessage; public IntPtr hIcon;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=128)] public string szTip;
    public uint dwState, dwStateMask;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=256)] public string szInfo;
    public uint uTimeoutOrVersion;
    [MarshalAs(UnmanagedType.ByValTStr, SizeConst=64)] public string szInfoTitle;
    public uint dwInfoFlags; public Guid guidItem; public IntPtr hBalloonIcon;
  }

  [STAThread]
  static void Main() {
    IntPtr tray = FindWindowW("Shell_TrayWnd", null);
    Console.WriteLine("Shell_TrayWnd = " + tray + "  IsWindow=" + IsWindow(tray));
    IntPtr h = CreateWindowExW(0, "STATIC", "probe", unchecked((int)0x80000000), 0,0,1,1, IntPtr.Zero, IntPtr.Zero, GetModuleHandle(null), IntPtr.Zero);
    Console.WriteLine("probe hwnd = " + h + " err=" + Marshal.GetLastWin32Error());
    var d = new NID();
    d.cbSize = Marshal.SizeOf(typeof(NID));
    d.hWnd = h;
    d.uID = 1;
    d.uFlags = 0x01 | 0x02 | 0x04;
    d.uCallbackMessage = 0x0400 + 2;
    d.hIcon = LoadIconW(IntPtr.Zero, new IntPtr(32512));   // IDI_APPLICATION
    d.szTip = "PROBE_TRAY";
    bool ok = Shell_NotifyIconW(0, ref d);
    Console.WriteLine("NIM_ADD = " + ok + " err=" + Marshal.GetLastWin32Error() + " cbSize=" + d.cbSize);
    if (ok) { System.Threading.Thread.Sleep(1500); Shell_NotifyIconW(2, ref d); Console.WriteLine("removed"); }
  }
}

