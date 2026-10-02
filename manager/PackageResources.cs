using System.IO.Compression;
using System.Reflection;
using System.Security.Cryptography;
using System.Text.Json;

namespace LuminexManager;

internal static class PackageResources
{
    private static string? _root;
    public static string Root => _root ??= Extract();

    private static string Extract()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("Luminex.Package.zip")
            ?? throw new InvalidOperationException("This development build has no installation resources. Run LuminexSetup.exe from the release build.");
        string root = Path.Combine(Path.GetTempPath(), "Luminex", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(root);
        try
        {
            ZipFile.ExtractToDirectory(stream, root);
            using var manifest = JsonDocument.Parse(File.ReadAllText(Path.Combine(root, "installer", "release-manifest.json")));
            foreach (var entry in manifest.RootElement.GetProperty("files").EnumerateArray())
            {
                string path = Path.GetFullPath(Path.Combine(root, entry.GetProperty("path").GetString()!));
                if (!path.StartsWith(root + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Invalid installation resource path.");
                using var file = File.OpenRead(path);
                if (!Convert.ToHexString(SHA256.HashData(file)).Equals(entry.GetProperty("sha256").GetString(), StringComparison.OrdinalIgnoreCase))
                    throw new InvalidDataException("Installation resources are damaged. Download LuminexSetup.exe again.");
            }
            return root;
        }
        catch { Directory.Delete(root, true); throw; }
    }

    public static void ShowLicenses(IWin32Window? owner = null)
    {
        using var dialog = new Form { Text = "Luminex - Third-party licenses", Width = 900, Height = 650,
            StartPosition = FormStartPosition.CenterParent, BackColor = Color.FromArgb(24, 23, 35), ForeColor = Color.White };
        var split = new SplitContainer { Width = 860, Dock = DockStyle.Fill, SplitterDistance = 260 };
        var list = new ListBox { Dock = DockStyle.Fill, HorizontalScrollbar = true, BackColor = dialog.BackColor, ForeColor = Color.White };
        var text = new TextBox { Dock = DockStyle.Fill, Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical,
            WordWrap = true, BackColor = dialog.BackColor, ForeColor = Color.White, Font = new Font("Consolas", 10) };
        string licenses = Path.Combine(Root, "Licenses");
        var files = Directory.GetFiles(licenses, "*", SearchOption.AllDirectories).Order().ToArray();
        foreach (string file in files) list.Items.Add(Path.GetRelativePath(licenses, file));
        list.SelectedIndexChanged += (_, _) => { if (list.SelectedIndex >= 0) text.Text = File.ReadAllText(files[list.SelectedIndex]).ReplaceLineEndings(Environment.NewLine); };
        split.Panel1.Controls.Add(list); split.Panel2.Controls.Add(text); dialog.Controls.Add(split);
        if (files.Length > 0) list.SelectedIndex = 0;
        dialog.ShowDialog(owner);
    }

    public static void Cleanup()
    {
        if (_root is null) return;
        try { Directory.Delete(_root, true); } catch (IOException) { } catch (UnauthorizedAccessException) { }
        _root = null;
    }
}
