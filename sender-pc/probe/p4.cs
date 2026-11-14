using System;
using System.Drawing;
using System.IO;
using System.Windows.Forms;

class P4 {
  [STAThread]
  static void Main() {
    var f = new Form();
    f.FormBorderStyle = FormBorderStyle.None;
    f.ShowInTaskbar = false;
    f.Opacity = 0;
    f.Show();
    var ni = new NotifyIcon();
    ni.Icon = SystemIcons.Information;
    ni.Text = "P4TEST";
    ni.Visible = true;
    File.WriteAllText(@"D:\DSH-Workplace\XingLianCast\sender-pc\probe\p4.log",
        "visible set at " + DateTime.Now.ToString("HH:mm:ss"));
    var t = new Timer();
    t.Interval = 4000;
    t.Tick += (s, e) => { t.Stop(); File.AppendAllText(@"D:\DSH-Workplace\XingLianCast\sender-pc\probe\p4.log", "\nexiting"); Application.Exit(); };
    t.Start();
    Application.Run(f);
  }
}
