using System;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;

namespace ZiiuApp.Backend;

public enum StreamState { Stopped, Starting, Running, Stopping }

// Starts, stops and resets the stream through the same pieces the scripts use:
//   start = tools\ziiu-drc\run.ps1 (hidden, its output in .local\stream\launcher.log)
//   stop  = ziiuctl stream-stop; run.ps1's own cleanup then stops the encoder and input
//   reset = stop, wait for the launcher mutex like reset.ps1, start again
// The launcher process is not tied to the app: closing the app leaves a stream running,
// and a stream started elsewhere (Reset connection.cmd, a console) is detected and stoppable.
public sealed class StreamController
{
    const string LauncherMutex = @"Local\ZiiUScreen.Launcher";   // held by run.ps1
    const string ResetMutex = @"Local\ZiiUScreen.Reset";         // held by reset.ps1

    readonly object gate = new();
    Process? launcher;
    DateTime launchedUtc;
    volatile bool stopping, stopRequested;
    int busy;

    public AppSettings? RunningSettings { get; private set; }
    public string LauncherLog => Repo.Log("launcher.log");

    // Set when the app's own launcher ended without being asked to; cleared on start.
    public string? EndedMessage { get; private set; }

    public bool Busy => Volatile.Read(ref busy) != 0;
    bool OwnedRunning { get { lock (gate) return launcher is { HasExited: false }; } }

    public StreamState State
    {
        get
        {
            Reap();
            if (stopping) return StreamState.Stopping;
            if (OwnedRunning) return RadioUp(launchedUtc) ? StreamState.Running : StreamState.Starting;
            if (LauncherActive() || Alive("ziiu-drc")) return StreamState.Running;
            return StreamState.Stopped;
        }
    }

    // A stream is running that this app instance did not start.
    public bool External => !OwnedRunning && (LauncherActive() || Alive("ziiu-drc"));

    public Task StartAsync(AppSettings s) => Guard(() => StartCore(s));
    public Task StopAsync() => Guard(StopCore);
    public Task ResetAsync(AppSettings s) => Guard(() => ResetCore(s));

    Task Guard(Action a)
    {
        if (Interlocked.Exchange(ref busy, 1) != 0) return Task.FromException(new InvalidOperationException("Another start/stop is still in progress."));
        return Task.Run(() => { try { a(); } finally { Volatile.Write(ref busy, 0); } });
    }

    void StartCore(AppSettings s)
    {
        if (OwnedRunning || LauncherActive()) throw new InvalidOperationException("A stream is already running.");
        foreach (string f in new[] { Repo.RunScript, Repo.Ctl, Repo.Encoder })
            if (!File.Exists(f)) throw new FileNotFoundException("Missing " + f + " (see docs/SETUP.md).");
        Directory.CreateDirectory(Repo.StreamLogs);
        string log = LauncherLog;
        string args = s.RunArguments();
        File.WriteAllText(log, $"ZiiUScreen app: starting run.ps1 {args} at {DateTime.Now:yyyy-MM-dd HH:mm:ss}\r\n");
        // The app is elevated (app.manifest), so run.ps1 and the processes it starts are too.
        // Its output goes to a file, never a pipe, so the stream outlives the app safely.
        // (Add-Content, not *>>: Windows PowerShell's redirection would append UTF-16.)
        string cmd = $"$ErrorActionPreference = 'Stop'; $log = {Ps.Quote(log)}; " +
                     $"try {{ & {Ps.Quote(Repo.RunScript)} {args} *>&1 | ForEach-Object {{ \"$_\" }} | Add-Content -LiteralPath $log -Encoding UTF8; exit 0 }} " +
                     "catch { ('FAILED: ' + $_.Exception.Message) | Add-Content -LiteralPath $log -Encoding UTF8; exit 1 }";
        var p = Process.Start(Ps.Start(cmd)) ?? throw new InvalidOperationException("Could not start PowerShell.");
        lock (gate)
        {
            launcher = p;
            launchedUtc = DateTime.UtcNow;
            RunningSettings = s.Clone();
            EndedMessage = null;
            stopRequested = false;
        }
    }

    void StopCore()
    {
        stopping = true;
        stopRequested = true;
        try
        {
            // stream-stop only works once ziiuctl has created its stop event (after the
            // firmware is up), so keep asking while a stream is still starting.
            DateTime deadline = DateTime.UtcNow.AddSeconds(40);
            while (DateTime.UtcNow < deadline)
            {
                if (!StreamAlive()) return;
                if (!LauncherActive() && !Alive("ziiuctl") && !OwnedRunning)
                {
                    KillOrphans();   // encoder/input left behind by a launcher that died
                    return;
                }
                RunCtlStop();
                for (int i = 0; i < 10 && StreamAlive(); i++) Thread.Sleep(200);
            }
            throw new TimeoutException("The stream did not stop within 40 s. Nothing was force-killed; see the radio log.");
        }
        finally { stopping = false; }
    }

    void ResetCore(AppSettings s)
    {
        using var reset = new Mutex(false, ResetMutex);
        bool ownsReset = false;
        try
        {
            try { ownsReset = reset.WaitOne(0); } catch (AbandonedMutexException) { ownsReset = true; }
            if (!ownsReset) throw new InvalidOperationException("A reset is already running.");
            StopCore();
            // Same wait as reset.ps1: the previous launcher must finish its cleanup.
            using var l = new Mutex(false, LauncherMutex);
            bool got;
            try { got = l.WaitOne(15000); } catch (AbandonedMutexException) { got = true; }
            if (!got) throw new TimeoutException("The previous stream has not finished cleanup; no new stream started.");
            l.ReleaseMutex();
        }
        finally { if (ownsReset) reset.ReleaseMutex(); }
        StartCore(s);
    }

    // Notice when the app's launcher has ended, and why.
    void Reap()
    {
        lock (gate)
        {
            if (launcher is not { HasExited: true } p) return;
            if (!stopRequested)
            {
                string tail = Repo.ReadTail(LauncherLog, 4096).Trim();
                string last = tail.Split('\n').LastOrDefault(l => l.Trim().Length > 0)?.Trim() ?? "";
                EndedMessage = p.ExitCode == 0 ? "The stream ended." : "The stream stopped with an error: " + last;
            }
            p.Dispose();
            launcher = null;
        }
    }

    bool StreamAlive() => OwnedRunning || LauncherActive() || Alive("ziiu-drc") || Alive("ziiu-input");

    static bool RadioUp(DateTime sinceUtc)
    {
        string radio = Repo.Log("radio.log");
        if (!File.Exists(radio) || File.GetLastWriteTimeUtc(radio) < sinceUtc.AddSeconds(-2)) return false;
        return Repo.ReadTail(radio, 8192).Contains("hostapd running");
    }

    public static bool LauncherActive()
    {
        try
        {
            if (!Mutex.TryOpenExisting(LauncherMutex, out Mutex? m)) return false;
            m.Dispose();
            return true;
        }
        catch (UnauthorizedAccessException) { return true; }   // exists, just not openable
    }

    static bool Alive(string name)
    {
        Process[] ps = Process.GetProcessesByName(name);
        foreach (var p in ps) p.Dispose();
        return ps.Length > 0;
    }

    static void RunCtlStop()
    {
        var psi = new ProcessStartInfo(Repo.Ctl, "stream-stop") { UseShellExecute = false, CreateNoWindow = true };
        using var p = Process.Start(psi);
        p?.WaitForExit(5000);
    }

    // Only this repo's encoder and input programs, and only when no launcher or radio is left
    // to clean them up (run.ps1 stops these two the same way).
    static void KillOrphans()
    {
        foreach (string name in new[] { "ziiu-drc", "ziiu-input" })
            foreach (var p in Process.GetProcessesByName(name))
            {
                using (p)
                {
                    try
                    {
                        string? path = p.MainModule?.FileName;
                        if (path != null && path.StartsWith(Repo.Root, StringComparison.OrdinalIgnoreCase)) p.Kill();
                    }
                    catch (Exception) { }
                }
            }
    }
}
