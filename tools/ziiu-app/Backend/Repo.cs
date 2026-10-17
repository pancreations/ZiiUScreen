using System;
using System.IO;
using System.Reflection;
using System.Text;

namespace ZiiuApp.Backend;

// Paths into the repo. The app runs from build\ziiu-app\bin\..., so it finds the repo by
// walking up to the folder that has tools\ziiu-drc\run.ps1 (ZIIU_REPO overrides;
// the repo path build.ps1 baked in is the fallback when the exe is copied elsewhere).
static class Repo
{
    public static string Root { get; } = Find();

    public static string RunScript => P("tools", "ziiu-drc", "run.ps1");
    public static string WifiModeScript => P("tools", "wifi-mode.ps1");
    public static string Ctl => P("build", "ziiuctl", "Release", "ziiuctl.exe");
    public static string Encoder => P(".local", "build", "ziiu-drc", "ziiu-drc.exe");
    public static string InputExe => P("build", "ziiu-input", "Release", "ziiu-input.exe");
    public static string StreamLogs => P(".local", "stream");
    public static string HostapdLog => P(".local", "gamepad-link", "hostapd.log");
    public static string SettingsFile => P(".local", "app", "settings.json");

    public static string P(params string[] parts) => Path.Combine([Root, .. parts]);
    public static string Log(string name) => Path.Combine(StreamLogs, name);

    static string Find()
    {
        string? env = Environment.GetEnvironmentVariable("ZIIU_REPO");
        if (!string.IsNullOrEmpty(env) && IsRepo(env)) return Path.GetFullPath(env);
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d != null; d = d.Parent)
            if (IsRepo(d.FullName)) return d.FullName;
        // Copied out of the repo (e.g. to the desktop): the path build.ps1 baked in.
        foreach (var a in typeof(Repo).Assembly.GetCustomAttributes<AssemblyMetadataAttribute>())
            if (a.Key == "ZiiuRepo" && a.Value != null && IsRepo(a.Value)) return a.Value;
        throw new InvalidOperationException("ZiiUScreen repo not found above " + AppContext.BaseDirectory +
                                            ". Set ZIIU_REPO to the repo folder.");
    }

    static bool IsRepo(string dir) => File.Exists(Path.Combine(dir, "tools", "ziiu-drc", "run.ps1"));

    // Last `bytes` of a log another process is still writing, starting at a line boundary.
    public static string ReadTail(string path, int bytes)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            long start = Math.Max(0, fs.Length - bytes);
            fs.Seek(start, SeekOrigin.Begin);
            var buf = new byte[fs.Length - start];
            int n = fs.ReadAtLeast(buf, buf.Length, throwOnEndOfStream: false);
            string text = DecodeLog(buf, n);
            if (start > 0)
            {
                int nl = text.IndexOf('\n');
                text = nl >= 0 ? text[(nl + 1)..] : "";
            }
            return text;
        }
        catch (IOException) { return ""; }
        catch (UnauthorizedAccessException) { return ""; }
    }

    public static string ReadHead(string path, int bytes)
    {
        try
        {
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var buf = new byte[Math.Min(bytes, fs.Length)];
            int n = fs.ReadAtLeast(buf, buf.Length, throwOnEndOfStream: false);
            return DecodeLog(buf, n);
        }
        catch (IOException) { return ""; }
        catch (UnauthorizedAccessException) { return ""; }
    }

    // The programs log UTF-8; PowerShell's *> redirection (stop.log, wifi-mode) writes UTF-16.
    static string DecodeLog(byte[] buf, int n)
    {
        if (n >= 2 && buf[0] == 0xFF && buf[1] == 0xFE) return Encoding.Unicode.GetString(buf, 2, n - 2);
        if (n >= 4 && buf[1] == 0 && buf[3] == 0) return Encoding.Unicode.GetString(buf, 0, n & ~1);
        return Encoding.UTF8.GetString(buf, 0, n);
    }
}
