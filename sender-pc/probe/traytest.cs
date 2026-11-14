using System;
using System.Drawing;
using System.Windows.Forms;
class T {
  [STAThread]
  static void Main() {
    var ni = new NotifyIcon();
    ni.Icon = SystemIcons.Information;
    ni.Text = "TRAYTEST_XYZ";
    ni.Visible = true;
    var menu = new ContextMenuStrip();
    menu.Items.Add("退出", null, (s, e) => Application.Exit());
    ni.ContextMenuStrip = menu;
    var f = new Form();
    f.Text = "TRAYTEST_FORM";
    f.WindowState = FormWindowState.Minimized;
    f.ShowInTaskbar = false;
    Application.Run(f);
  }
}
