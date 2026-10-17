using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading.Tasks;

namespace ZiiuApp.Backend;

// Hidden Windows PowerShell runs (the repo's scripts are written for powershell.exe 5.1).
static class Ps
{
    public static string Exe => Path.Combine(Environment.SystemDirectory, "WindowsPowerShell", "v1.0", "powershell.exe");

    public static string Quote(string s) => "'" + s.Replace("'", "''") + "'";

    public static ProcessStartInfo Start(string command)
    {
        var psi = new ProcessStartInfo(Exe)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            WorkingDirectory = Repo.Root,
        };
        foreach (string a in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-EncodedCommand" })
            psi.ArgumentList.Add(a);
        psi.ArgumentList.Add(Convert.ToBase64String(Encoding.Unicode.GetBytes(command)));
        return psi;
    }

    // Run a short command and return everything it printed.
    public static async Task<string> RunAsync(string command)
    {
        var psi = Start("[Console]::OutputEncoding = [Text.Encoding]::UTF8; & { " + command + " } *>&1 | Out-String -Width 200");
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.StandardOutputEncoding = Encoding.UTF8;
        psi.StandardErrorEncoding = Encoding.UTF8;
        using var p = Process.Start(psi)!;
        Task<string> err = p.StandardError.ReadToEndAsync();
        string text = await p.StandardOutput.ReadToEndAsync();
        await p.WaitForExitAsync();
        text += await err;
        return text.Trim();
    }
}
