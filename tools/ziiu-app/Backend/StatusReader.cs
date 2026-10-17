using System;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

namespace ZiiuApp.Backend;

// What the dashboard shows, read from the logs the stream already writes
// (.local\stream\*.log, .local\gamepad-link\hostapd.log). Nothing here talks to the
// GamePad; a value is null when the logs don't say.
public sealed record StreamStatus
{
    public DateTime? SessionStart;            // run.ps1 writes status.txt when it starts

    // hostapd: last AP-STA-CONNECTED / -DISCONNECTED of this session
    public bool? GamePadAssociated;
    public DateTime? GamePadEventTime;
    public string? GamePadMac;

    // ziiu-input stats (every 5 s)
    public double? InputReportsPerSec;
    public string? ControllerInfo;            // "virtual Xbox 360 controller added (position mapping)"
    public string? TouchInfo;
    public string? CalibrationInfo;
    public int? VolumeSlider;

    // ziiu-drc stats (every 300 frames, ~5 s)
    public bool EncoderFresh;
    public long? Frames;
    public double? EncodeAvgMs;
    public long? EncodeFailures;
    public long? Resyncs;
    public long? ResyncsRecent;               // between the last two stats lines
    public long? SkippedTicks;
    public double? Mbps;
    public string? CaptureInfo;               // "\\.\DISPLAY3 on NVIDIA ..., desktop 1920x1080"
    public string? AudioDevice;
    public int? AudioPeak;
    public long? AudioUnderruns;
    public string? RadioChannel;
}

public static class StatusReader
{
    static readonly Regex FramesRe = new(@"^frames=(\d+) \(enc avg ([\d.]+) ms, fail (\d+)\).*resyncs=(\d+)", RegexOptions.Multiline);
    static readonly Regex TimingRe = new(@"^timing: .*skipped ticks (\d+)", RegexOptions.Multiline);
    static readonly Regex VideoRe = new(@"^video: .*\(([\d.]+) Mbit/s\)", RegexOptions.Multiline);
    static readonly Regex AudioRe = new(@"^audio: (.+?), packets from PC \d+, peak (\d+) of 32767.*underruns (\d+)", RegexOptions.Multiline);
    static readonly Regex CaptureRe = new(@"^capture: (\\\\\.\\.+)$", RegexOptions.Multiline);
    static readonly Regex InputStatsRe = new(@"^stats: reports=(\d+).*volume_slider=(\d+)", RegexOptions.Multiline);
    static readonly Regex StaRe = new(@"^(\d+)\.\d+: \S+: AP-STA-(CONNECTED|DISCONNECTED) ([0-9a-f:]{17})", RegexOptions.Multiline);
    static readonly Regex ChannelRe = new(@"hostapd running on channel (\d+)");

    public static StreamStatus Read(bool streamRunning)
    {
        var st = new StreamStatus();
        if (!streamRunning) return st;
        st.SessionStart = SessionStart();
        DateTime since = (st.SessionStart ?? DateTime.MinValue).AddSeconds(-5);

        ReadEncoder(st, since);
        ReadInput(st, since);
        ReadHostapd(st, since);

        string radio = Repo.Log("radio.log");
        if (Fresh(radio, since))
        {
            Match m = ChannelRe.Match(Repo.ReadTail(radio, 8192));
            if (m.Success) st.RadioChannel = m.Groups[1].Value;
        }
        return st;
    }

    static DateTime? SessionStart()
    {
        string f = Repo.Log("status.txt");
        if (!File.Exists(f)) return null;
        // "running; ..." at start, "stopped" appended by the launcher's cleanup.
        string text = Repo.ReadHead(f, 1024);
        if (!text.StartsWith("running") || text.Contains("stopped")) return null;
        return File.GetLastWriteTime(f);
    }

    static bool Fresh(string path, DateTime since) => File.Exists(path) && File.GetLastWriteTime(path) >= since;

    static void ReadEncoder(StreamStatus st, DateTime since)
    {
        string log = Repo.Log("encoder.log");
        if (!Fresh(log, since)) return;
        st.EncoderFresh = DateTime.Now - File.GetLastWriteTime(log) < TimeSpan.FromSeconds(15);
        string head = Repo.ReadHead(log, 8192);
        Match c = CaptureRe.Match(head);
        if (c.Success) st.CaptureInfo = c.Groups[1].Value.Trim();

        string tail = Repo.ReadTail(log, 32768);
        var frames = FramesRe.Matches(tail);
        if (frames.Count > 0)
        {
            Match f = frames[^1];
            st.Frames = long.Parse(f.Groups[1].Value);
            st.EncodeAvgMs = double.Parse(f.Groups[2].Value, CultureInfo.InvariantCulture);
            st.EncodeFailures = long.Parse(f.Groups[3].Value);
            st.Resyncs = long.Parse(f.Groups[4].Value);
            st.ResyncsRecent = frames.Count > 1 ? st.Resyncs - long.Parse(frames[^2].Groups[4].Value) : st.Resyncs;
        }
        if (Last(TimingRe, tail) is { } t) st.SkippedTicks = long.Parse(t.Groups[1].Value);
        if (Last(VideoRe, tail) is { } v) st.Mbps = double.Parse(v.Groups[1].Value, CultureInfo.InvariantCulture);
        if (Last(AudioRe, tail) is { } a)
        {
            st.AudioDevice = a.Groups[1].Value;
            st.AudioPeak = int.Parse(a.Groups[2].Value);
            st.AudioUnderruns = long.Parse(a.Groups[3].Value);
        }
    }

    static void ReadInput(StreamStatus st, DateTime since)
    {
        string log = Repo.Log("input.log");
        if (!Fresh(log, since)) return;
        foreach (string line in Repo.ReadHead(log, 8192).Split('\n').Select(l => l.Trim()))
        {
            if (line.StartsWith("ziiu-input: virtual")) st.ControllerInfo = line["ziiu-input: ".Length..].Split(", input port")[0];
            else if (line.StartsWith("touch: display")) st.TouchInfo = line["touch: ".Length..];
            else if (line.StartsWith("calibration:")) st.CalibrationInfo = line["calibration: ".Length..];
        }
        var stats = InputStatsRe.Matches(Repo.ReadTail(log, 8192));
        if (stats.Count > 0) st.VolumeSlider = int.Parse(stats[^1].Groups[2].Value);
        // Two stats lines 5 s apart; a stale log (no new line for 12 s) means no reports.
        if (stats.Count > 1)
        {
            long d = long.Parse(stats[^1].Groups[1].Value) - long.Parse(stats[^2].Groups[1].Value);
            bool stale = DateTime.Now - File.GetLastWriteTime(log) > TimeSpan.FromSeconds(12);
            st.InputReportsPerSec = stale ? 0 : Math.Max(0, d) / 5.0;
        }
    }

    // hostapd.log grows by ~1 MB an hour and the connect event is near its start, so it is
    // scanned incrementally: only bytes added since the last read. ziiuctl truncates the
    // file for every stream, which restarts the scan.
    static readonly object HostapdLock = new();
    static long hostapdScanned;
    static DateTime hostapdCreated;
    static Match? hostapdLast;

    static void ReadHostapd(StreamStatus st, DateTime since)
    {
        string log = Repo.HostapdLog;
        st.GamePadAssociated = false;
        if (!Fresh(log, since)) return;
        Match? m;
        lock (HostapdLock)
        {
            try
            {
                using var fs = new FileStream(log, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                DateTime created = File.GetCreationTimeUtc(log);
                if (fs.Length < hostapdScanned || created != hostapdCreated)
                {
                    hostapdScanned = 0;
                    hostapdLast = null;
                    hostapdCreated = created;
                }
                fs.Seek(hostapdScanned, SeekOrigin.Begin);
                var buf = new byte[fs.Length - hostapdScanned];
                int n = fs.ReadAtLeast(buf, buf.Length, throwOnEndOfStream: false);
                int end = n > 0 ? Array.LastIndexOf(buf, (byte)'\n', n - 1) + 1 : 0;   // whole lines only
                if (n > 0 && end > 0)
                {
                    hostapdLast = Last(StaRe, System.Text.Encoding.UTF8.GetString(buf, 0, end)) ?? hostapdLast;
                    hostapdScanned += end;
                }
            }
            catch (IOException) { }
            m = hostapdLast;
        }
        if (m == null) return;
        st.GamePadAssociated = m.Groups[2].Value == "CONNECTED";
        st.GamePadEventTime = DateTimeOffset.FromUnixTimeSeconds(long.Parse(m.Groups[1].Value)).LocalDateTime;
        st.GamePadMac = m.Groups[3].Value;
    }

    static Match? Last(Regex re, string text)
    {
        var all = re.Matches(text);
        return all.Count > 0 ? all[^1] : null;
    }
}
