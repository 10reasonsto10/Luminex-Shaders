using Microsoft.Win32;
using System.Diagnostics;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;

namespace LuminexManager;

internal static class AppInstallation
{
    internal const string RegistryPath = @"Software\Microsoft\Windows\CurrentVersion\Uninstall\LuminexShaders";
    internal static string AppRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "LuminexShaders");
    internal static string ShortcutPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.Programs), "Luminex Shaders.lnk");
    internal static string DesktopShortcutPath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory), "Luminex Shaders.lnk");
    internal static bool ReadDesktopShortcutPreference(string registryPath = RegistryPath)
    {
        using var key = Registry.CurrentUser.OpenSubKey(registryPath);
        return key?.GetValue("CreateDesktopShortcut") is int value && value != 0;
    }

    internal static string? InstalledExecutable { get; private set; }

    internal static bool ReadShortcutPreference(string registryPath = RegistryPath)
    {
        using var key = Registry.CurrentUser.OpenSubKey(registryPath);
        return key?.GetValue("CreateStartMenuShortcut") is not int value || value != 0;
    }

    internal static string Register(string? appRoot = null, string? shortcutPath = null, string registryPath = RegistryPath, string? source = null, bool createStartMenuShortcut = true, bool createDesktopShortcut = false, string? desktopShortcutPath = null)
    {
        appRoot ??= AppRoot; shortcutPath ??= ShortcutPath; source ??= Environment.ProcessPath!;
        using var stream = File.OpenRead(source);
        string hash = Convert.ToHexString(SHA256.HashData(stream));
        string directory = Path.Combine(appRoot, hash[..16]);
        string executable = Path.Combine(directory, "Luminex.exe");
        Directory.CreateDirectory(directory);
        if (!File.Exists(executable)) File.Copy(source, executable);
        else
        {
            using var installed = File.OpenRead(executable);
            if (Convert.ToHexString(SHA256.HashData(installed)) != hash)
                throw new IOException("The installed manager is damaged. Close Luminex and run setup again.");
        }
        foreach (var (linkPath, enabled) in new[] { (shortcutPath, createStartMenuShortcut), (desktopShortcutPath ?? DesktopShortcutPath, createDesktopShortcut) })
        {
            if (enabled)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(linkPath)!);
                Type shellType = Type.GetTypeFromProgID("WScript.Shell") ?? throw new InvalidOperationException("Windows shortcuts are unavailable.");
                dynamic shell = Activator.CreateInstance(shellType)!;
                dynamic shortcut = shell.CreateShortcut(linkPath);
                try
                {
                    shortcut.TargetPath = executable; shortcut.Arguments = "--manager";
                    shortcut.WorkingDirectory = directory; shortcut.Description = "Luminex Shaders";
                    shortcut.IconLocation = executable + ",0"; shortcut.Save();
                }
                finally { System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shortcut); System.Runtime.InteropServices.Marshal.FinalReleaseComObject(shell); }
            }
            else if (File.Exists(linkPath)) File.Delete(linkPath);
        }
        using var key = Registry.CurrentUser.CreateSubKey(registryPath);
        key.SetValue("CreateStartMenuShortcut", createStartMenuShortcut ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("CreateDesktopShortcut", createDesktopShortcut ? 1 : 0, RegistryValueKind.DWord);
        key.SetValue("DisplayName", "Luminex Shaders");
        key.SetValue("DisplayVersion", typeof(AppInstallation).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion.Split('+')[0] ?? "0.1.0-alpha");
        key.SetValue("Publisher", "Luminex Shaders");
        key.SetValue("InstallLocation", appRoot);
        key.SetValue("DisplayIcon", executable);
        key.SetValue("UninstallString", $"\"{executable}\" --uninstall");
        key.SetValue("NoModify", 1, RegistryValueKind.DWord);
        key.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        InstalledExecutable = executable;
        return executable;
    }

    internal static void Unregister(string? shortcutPath = null, string registryPath = RegistryPath, string? desktopShortcutPath = null)
    {
        Registry.CurrentUser.DeleteSubKeyTree(registryPath, false);
        File.Delete(shortcutPath ?? ShortcutPath);
        if (File.Exists(desktopShortcutPath ?? DesktopShortcutPath)) File.Delete(desktopShortcutPath ?? DesktopShortcutPath);
    }

    internal static void CheckOtherManagers()
    {
        foreach (var process in Process.GetProcessesByName("Luminex"))
        {
            using (process)
            {
                if (process.Id == Environment.ProcessId) continue;
                try
                {
                    if (process.MainModule?.FileName?.StartsWith(AppRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) == true)
                        throw new IOException("Close the other Luminex manager windows before uninstalling.");
                }
                catch (System.ComponentModel.Win32Exception) { }
                catch (InvalidOperationException) { }
            }
        }
    }

    internal static void ScheduleRemoval(string? appRoot = null, string? shortcutPath = null, string registryPath = RegistryPath, int? waitProcessId = null, string? desktopShortcutPath = null)
    {
        string root = Path.GetFullPath(appRoot ?? AppRoot);
        string expectedParent = Path.GetFullPath(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs"));
        bool isolatedTest = appRoot != null && root.StartsWith(Path.Combine(Path.GetTempPath(), "LuminexSingleTest-"), StringComparison.OrdinalIgnoreCase)
            && registryPath.StartsWith(@"Software\LuminexTests\", StringComparison.Ordinal);
        if ((!isolatedTest && Path.GetDirectoryName(root) != expectedParent) || Path.GetFileName(root) != "LuminexShaders")
            throw new InvalidOperationException("Unexpected application removal path.");
        // Resolve and reject links before the helper performs any recursive deletion.
        for (var directory = new DirectoryInfo(root); directory != null && directory.Exists; directory = directory.Parent)
            if ((directory.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException("Cannot remove an application through a directory link.");
        string literal = root.Replace("'", "''");
        string shortcutLiteral = (shortcutPath ?? ShortcutPath).Replace("'", "''");
        string desktopLiteral = (desktopShortcutPath ?? DesktopShortcutPath).Replace("'", "''");
        string registryLiteral = registryPath.Replace("'", "''");
        string script = $$"""
            $ErrorActionPreference = 'Stop'
            Add-Type -AssemblyName System.Windows.Forms
            Wait-Process -Id {{waitProcessId ?? Environment.ProcessId}} -ErrorAction SilentlyContinue
            $target = '{{literal}}'
            while ($true) {
                $failure = $null
                for ($attempt = 0; $attempt -lt 30; $attempt++) {
                    try {
                        if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force -ErrorAction Stop }
                        $failure = $null
                        break
                    } catch { $failure = $_.Exception.Message; Start-Sleep -Seconds 1 }
                }
                if ($null -eq $failure) {
                    try {
                        if (Test-Path -LiteralPath '{{shortcutLiteral}}') { Remove-Item -LiteralPath '{{shortcutLiteral}}' -Force }
                        if (Test-Path -LiteralPath '{{desktopLiteral}}') { Remove-Item -LiteralPath '{{desktopLiteral}}' -Force }
                        [Microsoft.Win32.Registry]::CurrentUser.DeleteSubKeyTree('{{registryLiteral}}', $false)
                        break
                    } catch { $failure = $_.Exception.Message }
                }
                $message = "Luminex could not finish uninstalling.`r`n`r`nClose any remaining Luminex windows, then click Retry.`r`nIf you cancel, run LuminexSetup.exe again to repair the installation before trying to uninstall again.`r`n`r`nFolder: $target`r`nDetails: $failure"
                if ([System.Windows.Forms.MessageBox]::Show($message, 'Luminex uninstall incomplete', 'RetryCancel', 'Error') -ne 'Retry') { exit 1 }
            }
            """;
        var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, WorkingDirectory = Path.GetTempPath() };
        foreach (string argument in new[] { "-NoProfile", "-NonInteractive", "-STA", "-EncodedCommand", Convert.ToBase64String(Encoding.Unicode.GetBytes(script)) }) info.ArgumentList.Add(argument);
        using var helper = Process.Start(info) ?? throw new IOException("Could not schedule removal of the manager.");
    }
}
