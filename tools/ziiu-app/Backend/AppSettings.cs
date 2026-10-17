using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;

namespace ZiiuApp.Backend;

// Stream options the app passes to tools\ziiu-drc\run.ps1, plus app behaviour.
// Saved in .local\app\settings.json (machine-local, not committed). Defaults are
// run.ps1's own defaults, so a fresh install behaves exactly like the script.
public sealed class AppSettings
{
    public string Capture { get; set; } = "";
    public string Preset { get; set; } = "fast";
    public int Channel { get; set; } = 0;
    public int Rate { get; set; } = 54;
    public string Audio { get; set; } = "default";
    public bool SensorBar { get; set; }
    public bool InputLabels { get; set; }
    public bool InvertY { get; set; }
    public bool CloseToTray { get; set; } = true;

    // run.ps1's ValidateSet values.
    public static readonly string[] Presets = ["slow", "medium", "fast", "faster", "veryfast"];
    public static readonly int[] Channels = [36, 40, 44, 48, 149, 153, 157, 161, 165];
    public static readonly int[] Rates = [6, 9, 12, 18, 24, 36, 48, 54];

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true };

    public static AppSettings Load()
    {
        AppSettings s;
        try { s = JsonSerializer.Deserialize<AppSettings>(File.ReadAllText(Repo.SettingsFile)) ?? new(); }
        catch (Exception) { s = new(); }
        s.Normalize();
        return s;
    }

    public void Save()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Repo.SettingsFile)!);
        string tmp = Repo.SettingsFile + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(this, Json));
        File.Move(tmp, Repo.SettingsFile, overwrite: true);
    }

    void Normalize()
    {
        if (!Presets.Contains(Preset)) Preset = "fast";
        if (!Channels.Contains(Channel)) Channel = 0;
        if (!Rates.Contains(Rate)) Rate = 54;
        if (string.IsNullOrWhiteSpace(Capture)) Capture = "";
        if (string.IsNullOrWhiteSpace(Audio)) Audio = "default";
    }

    public AppSettings Clone() => (AppSettings)MemberwiseClone();

    // The options that only take effect when the stream (re)starts.
    public bool SameStreamOptions(AppSettings o) =>
        Capture == o.Capture && Preset == o.Preset && Channel == o.Channel && Rate == o.Rate &&
        Audio == o.Audio && SensorBar == o.SensorBar && InputLabels == o.InputLabels && InvertY == o.InvertY;

    // run.ps1 arguments as PowerShell source (strings single-quoted).
    public string RunArguments()
    {
        if (string.IsNullOrWhiteSpace(Capture)) throw new InvalidOperationException("Select your display before starting.");
        if (!Channels.Contains(Channel)) throw new InvalidOperationException("Select a supported 5 GHz channel permitted in your region.");
        var a = new List<string>
        {
            "-Capture", Ps.Quote(Capture), "-Preset", Preset, "-Channel", Channel.ToString(),
            "-Rate", Rate.ToString(), "-Audio", Ps.Quote(Audio),
        };
        if (SensorBar) a.Add("-SensorBar");
        if (InputLabels) a.Add("-InputLabels");
        if (InvertY) a.Add("-InvertY");
        return string.Join(' ', a);
    }
}
