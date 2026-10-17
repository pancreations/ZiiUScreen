using System;
using System.Drawing;
using System.Windows.Forms;

namespace ZiiuApp;

// Notification-area icon: open the window, start/stop/reset, exit.
sealed class Tray : IDisposable
{
    readonly NotifyIcon icon;
    readonly ToolStripMenuItem start, stop, reset;
    bool hintShown;

    public Tray(MainWindow w)
    {
        var menu = new ContextMenuStrip();
        menu.Items.Add("Open ZiiUScreen", null, (_, _) => w.BringBack());
        menu.Items.Add(new ToolStripSeparator());
        start = new ToolStripMenuItem("Start stream", null, (_, _) => w.StartStream());
        stop = new ToolStripMenuItem("Stop stream", null, (_, _) => w.StopStream());
        reset = new ToolStripMenuItem("Reset connection", null, (_, _) => w.ResetStream());
        menu.Items.AddRange([start, stop, reset]);
        menu.Items.Add(new ToolStripSeparator());
        menu.Items.Add("Exit", null, (_, _) => w.ExitApp());

        icon = new NotifyIcon { Text = "ZiiUScreen", Icon = LoadIcon(), ContextMenuStrip = menu, Visible = true };
        icon.MouseClick += (_, e) => { if (e.Button == MouseButtons.Left) w.BringBack(); };
    }

    public void Update(string status, bool canStart, bool canStop)
    {
        string text = "ZiiUScreen: " + status;
        icon.Text = text.Length > 127 ? text[..127] : text;
        start.Enabled = canStart;
        stop.Enabled = canStop;
        reset.Enabled = canStop;
    }

    public void HiddenHint()
    {
        if (hintShown) return;
        hintShown = true;
        icon.ShowBalloonTip(3000, "ZiiUScreen", "Still running here. Right-click for stream controls or Exit.", ToolTipIcon.None);
    }

    static Icon LoadIcon()
    {
        try
        {
            var res = System.Windows.Application.GetResourceStream(new Uri("pack://application:,,,/ZiiUScreen;component/Assets/ZIIUICON.png"));
            if (res == null) return SystemIcons.Application;
            using var src = new Bitmap(res.Stream);
            Size size = SystemInformation.SmallIconSize;
            using var small = new Bitmap(src, size.Width * 2 > 64 ? new Size(64, 64) : new Size(size.Width * 2, size.Height * 2));
            return Icon.FromHandle(small.GetHicon());
        }
        catch (Exception) { return SystemIcons.Application; }
    }

    public void Dispose()
    {
        icon.Visible = false;
        icon.Dispose();
    }
}
