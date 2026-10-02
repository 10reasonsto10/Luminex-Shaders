using System.Text;
using System.Text.RegularExpressions;

namespace LuminexManager;

internal static class LogSettings
{
    internal const string Explanation = "Logs help diagnose loading, graphics and shader errors. They may contain timestamps, process/thread IDs, graphics device details, shader names and file paths (including your Windows username).\n\n"
        + "They stay on this PC. Luminex does not upload them. You choose whether to share them for troubleshooting. The manager's activity messages are only shown in its window.\n\n"
        + "Files: Runtime/Logs/Luminex.log and Runtime/ReShade.log. Each is replaced when its component next starts with logging enabled. Turning logging off leaves existing files in place; Uninstall's Delete logs option removes them.\n\n"
        + "Changes apply after closing the client and loading Luminex into a new session. This controls diagnostic log files, not your settings, presets, shader cache or screenshots.";

    internal static string PathFor(string root) => Path.Combine(root, "Runtime", "Config", "Diagnostics.ini");
    internal static bool Read(string root)
    {
        string path = PathFor(root);
        if (!File.Exists(path)) return true;
        return !Regex.IsMatch(File.ReadAllText(path), @"(?m)^SaveLogs\s*=\s*0\s*$");
    }
    internal static void Save(string root, bool enabled)
    {
        string path = PathFor(root);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temporary, $"[Diagnostics]\r\nSaveLogs={(enabled ? 1 : 0)}\r\n", new UTF8Encoding(false));
            File.Move(temporary, path, true);
        }
        finally { if (File.Exists(temporary)) File.Delete(temporary); }
    }
}
