using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading.Tasks;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Threading;
using ZiiuApp.Backend;

namespace ZiiuApp;

public partial class MainWindow : Window
{
    readonly StreamController stream = new();
    readonly AppSettings settings = AppSettings.Load();
    readonly DispatcherTimer timer = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Tray tray;
    bool loading, ticking, exiting;
    StreamState lastState = StreamState.Stopped;

    // Message bar
    string? bannerText, dismissed, shownEnded;
    Action? bannerAction;
    bool bannerIsError;

    static readonly (string Label, string File)[] Logs =
    [
        ("Launcher (app)", "launcher.log"), ("Encoder", "encoder.log"), ("Controller & touch", "input.log"),
        ("Radio", "radio.log"), ("Access point (hostapd)", "hostapd"), ("Reset", "reset.log"),
        ("Encoder errors", "encoder.err"), ("Controller errors", "input.err"), ("Radio errors", "radio.err"),
    ];

    public MainWindow()
    {
        InitializeComponent();
        Nav.SelectedIndex = 0;
        tray = new Tray(this);
        LoadSettingsIntoUi();
        foreach (var (label, file) in Logs) LogBox.Items.Add(new ComboBoxItem { Content = label, Tag = file });
        LogBox.SelectedIndex = 1;   // encoder.log; the launcher log only exists once the app started a stream
        string version = typeof(MainWindow).Assembly.GetName().Version?.ToString(3) ?? "";
        AboutText.Text = $"ZiiUScreen app {version} · repo {Repo.Root} · settings {Repo.SettingsFile}";
        Application.Current.SessionEnding += (_, _) => exiting = true;   // never block sign-out
        timer.Tick += async (_, _) => await Tick();
        timer.Start();
        Loaded += async (_, _) => await Tick();
    }

    // ---------------------------------------------------------------- settings

    static ComboBoxItem Item(string text, object tag) => new() { Content = text, Tag = tag };

    static void Select(ComboBox box, object tag)
    {
        foreach (ComboBoxItem i in box.Items)
            if (Equals(i.Tag, tag)) { box.SelectedItem = i; return; }
    }

    static T? Selected<T>(ComboBox box) => box.SelectedItem is ComboBoxItem { Tag: T t } ? t : default;

    void LoadSettingsIntoUi()
    {
        loading = true;
        try
        {
            FillDisplays();
            PresetBox.Items.Clear();
            foreach (string p in AppSettings.Presets) PresetBox.Items.Add(Item(p == "fast" ? "fast (default)" : p, p));
            Select(PresetBox, settings.Preset);
            FillAudio();
            ChannelBox.Items.Clear();
            ChannelBox.Items.Add(Item("Choose a locally permitted channel", 0));
            foreach (int c in AppSettings.Channels) ChannelBox.Items.Add(Item(c.ToString(), c));
            Select(ChannelBox, settings.Channel);
            RateBox.Items.Clear();
            foreach (int r in AppSettings.Rates) RateBox.Items.Add(Item(r == 54 ? "54 (default)" : r.ToString(), r));
            Select(RateBox, settings.Rate);
            LayoutBox.Items.Clear();
            LayoutBox.Items.Add(Item("By position: the bottom button (B) acts as Xbox A, as games' prompts expect (default)", false));
            LayoutBox.Items.Add(Item("By printed letter: GamePad A acts as Xbox A", true));
            Select(LayoutBox, settings.InputLabels);
            InvertYBox.IsChecked = settings.InvertY;
            SensorBarBox.IsChecked = settings.SensorBar;
            CloseToTrayBox.IsChecked = settings.CloseToTray;
        }
        finally { loading = false; }
    }

    void FillDisplays()
    {
        DisplayBox.Items.Clear();
        List<DisplayInfo> displays = DisplayList.Active();
        foreach (DisplayInfo d in displays) DisplayBox.Items.Add(Item(d.ToString(), d.HardwareId));
        if (!displays.Any(d => d.HardwareId.Equals(settings.Capture, StringComparison.OrdinalIgnoreCase)))
            DisplayBox.Items.Add(Item(string.IsNullOrWhiteSpace(settings.Capture) ? "Choose your display" : $"{settings.Capture}  ·  not connected right now", settings.Capture));
        foreach (ComboBoxItem i in DisplayBox.Items)
            if (string.Equals((string)i.Tag, settings.Capture, StringComparison.OrdinalIgnoreCase)) DisplayBox.SelectedItem = i;
    }

    void FillAudio()
    {
        AudioBox.Items.Clear();
        AudioBox.Items.Add(Item("Windows default playback device (default)", "default"));
        AudioBox.Items.Add(Item("The streamed display's HDMI audio output", "display"));
        foreach (string name in AudioDevices.ActiveOutputs()) AudioBox.Items.Add(Item(name, name));
        AudioBox.Items.Add(Item("Off: send no sound", "off"));
        if (!AudioBox.Items.Cast<ComboBoxItem>().Any(i => (string)i.Tag == settings.Audio))
            AudioBox.Items.Add(Item($"{settings.Audio}  ·  not found right now", settings.Audio));
        Select(AudioBox, settings.Audio);
    }

    void Setting_Changed(object sender, RoutedEventArgs e)
    {
        if (loading) return;
        settings.Capture = Selected<string>(DisplayBox) ?? settings.Capture;
        settings.Preset = Selected<string>(PresetBox) ?? settings.Preset;
        settings.Audio = Selected<string>(AudioBox) ?? settings.Audio;
        if (ChannelBox.SelectedItem is ComboBoxItem { Tag: int channel }) settings.Channel = channel;
        if (RateBox.SelectedItem is ComboBoxItem { Tag: int rate }) settings.Rate = rate;
        if (LayoutBox.SelectedItem is ComboBoxItem { Tag: bool labels }) settings.InputLabels = labels;
        settings.InvertY = InvertYBox.IsChecked == true;
        settings.SensorBar = SensorBarBox.IsChecked == true;
        settings.CloseToTray = CloseToTrayBox.IsChecked == true;
        try { settings.Save(); }
        catch (Exception ex) { ShowBanner("Couldn't save settings: " + ex.Message, error: true); }
        UpdateBanner();
    }

    void RefreshDisplays_Click(object sender, RoutedEventArgs e)
    {
        loading = true;
        try { FillDisplays(); FillAudio(); }
        finally { loading = false; }
    }

    // ---------------------------------------------------------------- stream

    void Start_Click(object sender, RoutedEventArgs e) => StartStream();
    void Stop_Click(object sender, RoutedEventArgs e) => StopStream();
    void Reset_Click(object sender, RoutedEventArgs e) => ResetStream();

    public void StartStream() => _ = Run(() => stream.StartAsync(settings.Clone()));
    public void StopStream() => _ = Run(stream.StopAsync);
    public void ResetStream() => _ = Run(() => stream.ResetAsync(settings.Clone()));

    async Task Run(Func<Task> action)
    {
        ClearBanner();
        Task t = action();
        await Tick();
        try { await t; }
        catch (Exception ex) { ShowBanner(ex.Message, error: true); }
        await Tick();
    }

    async Task Tick()
    {
        if (ticking) return;
        ticking = true;
        try
        {
            var (state, external, status) = await Task.Run(() =>
            {
                StreamState s = stream.State;
                return (s, stream.External, StatusReader.Read(s != StreamState.Stopped));
            });
            lastState = state;
            Render(state, external, status);
            if (PageLogs.Visibility == Visibility.Visible && LogFollow.IsChecked == true) RefreshLog();
        }
        catch (Exception ex) { ShowBanner("Status update failed: " + ex.Message, error: true); }
        finally { ticking = false; }
    }

    void Render(StreamState state, bool external, StreamStatus st)
    {
        bool busy = stream.Busy;
        bool running = state == StreamState.Running;
        bool padUp = running && st.GamePadAssociated == true;
        bool inputLive = st.InputReportsPerSec > 0;

        Brush tone = state switch
        {
            StreamState.Stopped => Res("IdleBrush"),
            StreamState.Running when padUp && inputLive => Res("OkBrush"),
            _ => Res("WarnBrush"),
        };
        string uptime = st.SessionStart is { } since ? " · running " + Span(DateTime.Now - since) : "";
        string origin = external ? " · started outside the app" : "";
        (string title, string detail) = state switch
        {
            StreamState.Stopped => ("Stream stopped", "Start the stream to put the desktop on the GamePad."),
            StreamState.Starting => ("Starting…", "Loading the radio firmware and opening the GamePad's access point."),
            StreamState.Stopping => ("Stopping…", "Shutting down the radio, encoder and controller."),
            _ when padUp && inputLive => ("Streaming to the GamePad", $"Connected since {st.GamePadEventTime:HH:mm}{uptime}{origin}"),
            _ when padUp => ("GamePad connected, no input yet", "The GamePad has joined; its input reports haven't arrived." + uptime + origin),
            _ => ("Waiting for the GamePad", "The access point is up. Turn the GamePad on; it connects with the pairing from your Wii U." + uptime + origin),
        };
        HeroState.Text = title;
        HeroDetail.Text = detail;
        HeroRing.Fill = tone;
        HeroIcon.Foreground = tone;
        SideDot.Fill = tone;
        SideState.Text = state switch
        {
            StreamState.Running => padUp ? "Streaming" : "Waiting for GamePad",
            _ => state.ToString(),
        };
        SidePad.Text = running ? (padUp ? "GamePad connected" : "GamePad not connected") : "GamePad: —";

        StartButton.IsEnabled = !busy && state == StreamState.Stopped;
        StopButton.IsEnabled = !busy && state is StreamState.Running or StreamState.Starting;
        ResetButton.IsEnabled = !busy && state is StreamState.Running or StreamState.Starting;
        tray.Update(SideState.Text, StartButton.IsEnabled, StopButton.IsEnabled);

        // GamePad tile
        if (!running) { PadValue.Text = "—"; PadDetail.Text = "Stream not running"; }
        else
        {
            PadValue.Text = padUp ? "Connected" : "Not connected";
            var lines = new List<string>();
            if (st.GamePadEventTime is { } t) lines.Add($"{(padUp ? "Joined" : "Left")} at {t:HH:mm:ss} · {st.GamePadMac}");
            if (st.RadioChannel != null) lines.Add($"Access point on channel {st.RadioChannel}");
            if (st.VolumeSlider is { } vol && padUp) lines.Add($"Volume slider {vol * 100 / 255}%");
            PadDetail.Text = string.Join("\n", lines);
        }

        // Video tile
        if (!running || st.Frames == null) { VideoValue.Text = "—"; VideoDetail.Text = running ? "Waiting for encoder stats" : "Stream not running"; }
        else
        {
            VideoValue.Text = st.EncoderFresh ? (st.Mbps is { } mb ? $"{mb:0.0} Mbit/s" : "Encoding") : "Encoder log not updating";
            VideoDetail.Text = string.Join("\n", new[]
            {
                $"Encode {st.EncodeAvgMs:0.0} ms avg · {st.EncodeFailures} failures · {st.SkippedTicks} skipped ticks",
                $"Resyncs: {st.ResyncsRecent} in the last 5 s · {st.Resyncs} total",
                st.CaptureInfo != null ? "Capturing " + st.CaptureInfo.Replace(@"\\.\", "") : null,
            }.Where(l => l != null));
        }

        // Controller tile
        if (!running) { InputValue.Text = "—"; InputDetail.Text = "Stream not running"; }
        else
        {
            InputValue.Text = st.InputReportsPerSec == null ? "Starting" : inputLive ? $"{st.InputReportsPerSec:0} reports/s" : "No input";
            InputDetail.Text = string.Join("\n", new[]
            {
                st.ControllerInfo != null ? Cap(st.ControllerInfo) : "Controller program not running",
                st.TouchInfo != null ? "Touch on " + st.TouchInfo : null,
                st.CalibrationInfo != null ? "Calibration " + st.CalibrationInfo : null,
            }.Where(l => l != null));
        }

        // Audio tile
        if (!running) { AudioValue.Text = "—"; AudioDetail.Text = "Stream not running"; }
        else if (settings.Audio == "off" && st.AudioDevice == null) { AudioValue.Text = "Off"; AudioDetail.Text = ""; }
        else
        {
            AudioValue.Text = st.AudioDevice ?? "Starting";
            AudioDetail.Text = (st.AudioPeak is { } pk ? $"Captured peak {pk} of 32767 · {st.AudioUnderruns} underruns\n" : "") +
                               "GamePad speakers are silent for now (known issue)";
        }

        // Pages that act on a running stream or need it stopped
        foreach (Button b in new[] { CalibrateButton, RumbleButton, IrOnButton, IrOffButton }) b.IsEnabled = running;
        LiveHint.Text = running ? "These act on the running stream." : "Start the stream to use these.";
        bool wifiBusy = WifiOutput.Tag is true;
        WifiStatusButton.IsEnabled = !wifiBusy;
        WifiNormalButton.IsEnabled = WifiGamePadButton.IsEnabled = !wifiBusy && state == StreamState.Stopped && !busy;

        // The launcher ended by itself: say so once.
        if (stream.EndedMessage is { } ended && ended != shownEnded)
        {
            shownEnded = ended;
            ShowBanner(ended + " See Logs for details.", error: !ended.EndsWith("ended."));
        }
        UpdateBanner();
    }

    static string Span(TimeSpan t) =>
        t.TotalHours >= 1 ? $"{(int)t.TotalHours} h {t.Minutes} min" : t.TotalMinutes >= 1 ? $"{t.Minutes} min" : $"{t.Seconds} s";

    static string Cap(string s) => s.Length > 0 ? char.ToUpper(s[0]) + s[1..] : s;

    Brush Res(string key) => (Brush)FindResource(key);

    // ---------------------------------------------------------------- message bar

    void ShowBanner(string text, bool error, string? actionLabel = null, Action? action = null)
    {
        if (!error && text == dismissed) return;
        bannerText = text;
        bannerIsError = error;
        bannerAction = action;
        BannerText.Text = text;
        BannerIcon.Text = error ? "" : "";
        Color c = error ? Color.FromRgb(0xE5, 0x53, 0x4B) : Color.FromRgb(0x3B, 0x82, 0xF6);
        BannerIcon.Foreground = new SolidColorBrush(c);
        Banner.Background = new SolidColorBrush(Color.FromArgb(0x22, c.R, c.G, c.B));
        Banner.BorderBrush = new SolidColorBrush(Color.FromArgb(0x66, c.R, c.G, c.B));
        BannerAction.Content = actionLabel;
        BannerAction.Visibility = actionLabel != null ? Visibility.Visible : Visibility.Collapsed;
        Banner.Visibility = Visibility.Visible;
    }

    void ClearBanner()
    {
        bannerText = null;
        bannerIsError = false;
        Banner.Visibility = Visibility.Collapsed;
    }

    // "Restart to apply" while the app's own stream runs with older settings.
    void UpdateBanner()
    {
        const string apply = "Settings changed. Reset the connection to apply them to the running stream.";
        bool stale = lastState == StreamState.Running && stream.RunningSettings is { } used && !used.SameStreamOptions(settings);
        if (stale && bannerText == null) ShowBanner(apply, error: false, "Reset now", ResetStream);
        else if (!stale && bannerText == apply) ClearBanner();
    }

    void BannerAction_Click(object sender, RoutedEventArgs e) => bannerAction?.Invoke();

    void BannerClose_Click(object sender, RoutedEventArgs e)
    {
        if (!bannerIsError) dismissed = bannerText;
        ClearBanner();
    }

    // ---------------------------------------------------------------- controller tools

    void Calibrate_Click(object sender, RoutedEventArgs e) =>
        Send(GamePadControl.Calibrate, "Calibration screen opened: follow it on the GamePad.");

    void Rumble_Click(object sender, RoutedEventArgs e) => Send(() => GamePadControl.Rumble(255, 400), null);
    void IrOn_Click(object sender, RoutedEventArgs e) => Send(() => GamePadControl.SensorBar(true), null);
    void IrOff_Click(object sender, RoutedEventArgs e) => Send(() => GamePadControl.SensorBar(false), null);

    void Send(Action a, string? note)
    {
        try { a(); if (note != null) ShowBanner(note, error: false); }
        catch (Exception ex) { ShowBanner("Couldn't reach the stream: " + ex.Message, error: true); }
    }

    // ---------------------------------------------------------------- Wi-Fi card

    async void WifiStatus_Click(object sender, RoutedEventArgs e) => await WifiMode("Status");

    async void WifiNormal_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "Switch the MT7925 back to MediaTek's driver (normal Wi-Fi)? The GamePad can't connect until you switch back.",
                            "ZiiUScreen", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
            await WifiMode("Normal");
    }

    async void WifiGamePad_Click(object sender, RoutedEventArgs e)
    {
        if (MessageBox.Show(this, "Bind the ZiiUScreen driver to the MT7925 (GamePad mode)? Normal Wi-Fi on that card stops.",
                            "ZiiUScreen", MessageBoxButton.OKCancel, MessageBoxImage.Question) == MessageBoxResult.OK)
            await WifiMode("GamePad");
    }

    async Task WifiMode(string mode)
    {
        WifiOutput.Tag = true;
        WifiOutput.Visibility = Visibility.Visible;
        WifiOutput.Text = $"Running wifi-mode.ps1 {mode}…";
        await Tick();
        try { WifiOutput.Text = await Ps.RunAsync($"& {Ps.Quote(Repo.WifiModeScript)} {mode}"); }
        catch (Exception ex) { WifiOutput.Text = "Failed: " + ex.Message; }
        finally { WifiOutput.Tag = null; }
        await Tick();
    }

    // ---------------------------------------------------------------- logs

    string LogPath(string file) => file == "hostapd" ? Repo.HostapdLog : Repo.Log(file);

    void LogBox_SelectionChanged(object sender, SelectionChangedEventArgs e) => RefreshLog(force: true);
    void RefreshLog_Click(object sender, RoutedEventArgs e) => RefreshLog(force: true);

    void RefreshLog(bool force = false)
    {
        if (Selected<string>(LogBox) is not { } file) return;
        string path = LogPath(file);
        string text = File.Exists(path) ? Repo.ReadTail(path, 96 * 1024) : $"{path} doesn't exist yet.";
        if (!force && text == LogText.Text) return;
        LogText.Text = text;
        if (LogFollow.IsChecked == true || force) LogText.ScrollToEnd();
    }

    void OpenLogs_Click(object sender, RoutedEventArgs e)
    {
        Directory.CreateDirectory(Repo.StreamLogs);
        Process.Start(new ProcessStartInfo("explorer.exe", Repo.StreamLogs) { UseShellExecute = true });
    }

    void OpenRepo_Click(object sender, RoutedEventArgs e) =>
        Process.Start(new ProcessStartInfo("explorer.exe", Repo.Root) { UseShellExecute = true });

    // ---------------------------------------------------------------- navigation, window

    void Nav_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (Nav.SelectedItem is not ListBoxItem { Tag: string page }) return;
        foreach (var (name, el) in new (string, FrameworkElement)[]
                 { ("Dashboard", PageDashboard), ("Display", PageDisplay), ("Controller", PageController),
                   ("Audio", PageAudio), ("Advanced", PageAdvanced), ("Logs", PageLogs) })
            el.Visibility = name == page ? Visibility.Visible : Visibility.Collapsed;
        if (page == "Logs") RefreshLog(force: true);
    }

    public void BringBack()
    {
        Show();
        if (WindowState == WindowState.Minimized) WindowState = WindowState.Normal;
        Activate();
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (!exiting && settings.CloseToTray)
        {
            e.Cancel = true;
            Hide();
            tray.HiddenHint();
            return;
        }
        if (!exiting)
        {
            e.Cancel = true;
            ExitApp();
            return;
        }
        base.OnClosing(e);
    }

    public async void ExitApp()
    {
        if (lastState != StreamState.Stopped)
        {
            BringBack();
            var answer = MessageBox.Show(this, "The stream is still running. Stop it before closing?\n\nNo leaves it running; you can stop it later from the app or with run.ps1 -Stop.",
                                         "ZiiUScreen", MessageBoxButton.YesNoCancel, MessageBoxImage.Question);
            if (answer == MessageBoxResult.Cancel) return;
            if (answer == MessageBoxResult.Yes)
            {
                try { await stream.StopAsync(); }
                catch (Exception ex)
                {
                    MessageBox.Show(this, ex.Message, "ZiiUScreen", MessageBoxButton.OK, MessageBoxImage.Warning);
                    return;
                }
            }
        }
        exiting = true;
        timer.Stop();
        tray.Dispose();
        Close();
        Application.Current.Shutdown();
    }
}
