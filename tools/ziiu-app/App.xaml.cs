using System;
using System.Threading;
using System.Windows;

namespace ZiiuApp;

public partial class App : Application
{
    const string InstanceMutex = @"Local\ZiiUScreen.App";
    const string ShowEvent = @"Local\ZiiUScreen.App.Show";

    Mutex? instance;
    EventWaitHandle? show;

    protected override void OnStartup(StartupEventArgs e)
    {
        base.OnStartup(e);
        // One app at a time: a second launch just brings the first one's window back.
        instance = new Mutex(true, InstanceMutex, out bool first);
        show = new EventWaitHandle(false, EventResetMode.AutoReset, ShowEvent);
        if (!first)
        {
            show.Set();
            Shutdown();
            return;
        }

        MainWindow window;
        try { window = new MainWindow(); }
        catch (Exception ex)
        {
            MessageBox.Show(ex.Message, "ZiiUScreen", MessageBoxButton.OK, MessageBoxImage.Error);
            Shutdown(1);
            return;
        }
        MainWindow = window;
        bool minimized = Array.Exists(e.Args, a => a.Equals("--tray", StringComparison.OrdinalIgnoreCase));
        if (!minimized) window.Show();

        var waiter = new Thread(() =>
        {
            while (show.WaitOne())
                Dispatcher.BeginInvoke(window.BringBack);
        }) { IsBackground = true, Name = "show-listener" };
        waiter.Start();
    }

    protected override void OnExit(ExitEventArgs e)
    {
        instance?.Dispose();
        base.OnExit(e);
    }
}
