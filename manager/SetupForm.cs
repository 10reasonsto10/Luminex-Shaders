using System.Diagnostics;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace LuminexManager;

internal sealed record Shortcut(int Key, bool Ctrl = false, bool Shift = false, bool Alt = false)
{
    public string Value => $"{Key},{(Ctrl ? 1 : 0)},{(Shift ? 1 : 0)},{(Alt ? 1 : 0)}";
    public string Display => Key == 0 ? "Not set" : string.Join(" + ",
        new[] { Ctrl ? "Ctrl" : null, Shift ? "Shift" : null, Alt ? "Alt" : null, KeyName(Key) }.Where(s => s != null));
    private static string KeyName(int key) => (Keys)key switch
    {
        Keys.Return => "Enter", Keys.Back => "Backspace", Keys.Escape => "Esc",
        Keys.Prior => "Page Up", Keys.Next => "Page Down", Keys.Capital => "Caps Lock",
        Keys.Oemtilde => "`", Keys.OemMinus => "-", Keys.Oemplus => "=",
        Keys.OemOpenBrackets => "[", Keys.OemCloseBrackets => "]", Keys.OemPipe => "\\",
        Keys.OemSemicolon => ";", Keys.OemQuotes => "'", Keys.Oemcomma => ",",
        Keys.OemPeriod => ".", Keys.OemQuestion => "/",
        >= Keys.D0 and <= Keys.D9 => ((char)('0' + key - (int)Keys.D0)).ToString(),
        _ => ((Keys)key).ToString()
    };
    public static Shortcut Parse(string text, Shortcut fallback)
    {
        var parts = text.Split(',');
        return parts.Length == 4 && int.TryParse(parts[0], out int key) && key is >= 0 and <= 255 &&
            parts.Skip(1).All(p => p is "0" or "1")
            ? new(key, parts[1] == "1", parts[2] == "1", parts[3] == "1") : fallback;
    }
}

internal sealed class CaptureShortcutForm : Form
{
    public Shortcut? Binding { get; private set; }
    public CaptureShortcutForm()
    {
        Text = "Choose a shortcut";
        ClientSize = new Size(440, 170);
        StartPosition = FormStartPosition.CenterParent;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MinimizeBox = MaximizeBox = false;
        Font = new Font("Segoe UI", 11);
        Controls.Add(new Label { Dock = DockStyle.Fill, Padding = new Padding(24),
            Text = "Press the key you want to use.\nYou can hold Ctrl, Shift or Alt with it.\n\nPress Esc to keep your current shortcut." });
        KeyPreview = true;
        KeyDown += (_, e) => { CaptureKey(e.KeyData); e.Handled = e.SuppressKeyPress = true; };
    }
    private void CaptureKey(Keys data)
    {
        Keys key = data & Keys.KeyCode;
        if (key == Keys.Escape) { DialogResult = DialogResult.Cancel; return; }
        if (key is Keys.None or Keys.ControlKey or Keys.ShiftKey or Keys.Menu or Keys.LWin or Keys.RWin) return;
        Binding = new((int)key, data.HasFlag(Keys.Control), data.HasFlag(Keys.Shift), data.HasFlag(Keys.Alt));
        DialogResult = DialogResult.OK;
    }
    protected override bool ProcessCmdKey(ref Message msg, Keys keyData) { CaptureKey(keyData); return true; }
}

internal sealed class SetupForm : Form
{
    internal static string InstallRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LuminexShaders");
    private readonly string _mode;
    private readonly string _installRoot;
    private readonly string _packageRoot;
    private readonly bool _integrateApplication;
    private readonly List<Control> _pages = [];
    private readonly Panel _content = new() { Dock = DockStyle.Fill, Padding = new Padding(24, 12, 24, 12) };
    private readonly Label _heading = new() { Dock = DockStyle.Top, Height = 72, Padding = new Padding(24, 20, 24, 0), Font = new Font("Segoe UI Semibold", 19) };
    private readonly Button _back = Button("Back");
    private readonly Button _next = Button("Next");
    private readonly Button _close = Button("Cancel");
    private readonly Button _open = Button("Open manager");
    private readonly Label _status = new() { AutoSize = false, Height = 80, Dock = DockStyle.Top };
    private readonly ProgressBar _progress = new() { Dock = DockStyle.Top, Height = 18 };
    private readonly TextBox _details = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Both, WordWrap = false, Dock = DockStyle.Fill, Visible = false };
    private readonly CheckedListBox _packs = new() { Dock = DockStyle.Fill, CheckOnClick = true, IntegralHeight = false,
        BackColor = Color.FromArgb(35, 33, 47), ForeColor = Color.WhiteSmoke, BorderStyle = BorderStyle.FixedSingle };
    private readonly CheckBox _settings = new() { Text = "Delete my settings and presets", AutoSize = true };
    private readonly CheckBox _shaders = new() { Text = "Delete downloaded shader packs and textures", AutoSize = true };
    private readonly CheckBox _logs = new() { Text = "Delete logs", AutoSize = true };
    private readonly CheckBox _everything = new() { Text = "Delete everything in the Luminex installation folder", AutoSize = true };
    private readonly CheckBox _saveLogs = new() { Text = "Save diagnostic logs on this PC", AutoSize = true, Margin = new Padding(0, 8, 0, 8) };
    private readonly CheckBox _startMenuShortcut = new() { Text = "Create Start menu shortcut", AutoSize = true, Checked = true, Margin = new Padding(0, 8, 0, 8) };
    private readonly CheckBox _desktopShortcut = new() { Text = "Create desktop shortcut", AutoSize = true, Margin = new Padding(0, 8, 0, 8) };
    private readonly List<Pack> _catalog = [];
    private Shortcut _menu = new((int)Keys.Home), _effects = new(0);
    private int _page;
    private bool _busy, _completed, _baseInstalled;
    public bool OpenManagerRequested { get; private set; }
    private sealed record Pack(string Id, string Name, string Description, string Url)
    {
        public override string ToString() => Name;
    }

    public SetupForm(string mode, string? destinationRoot = null, string? packageRoot = null)
    {
        _mode = mode;
        _installRoot = destinationRoot ?? InstallRoot;
        _packageRoot = packageRoot ?? PackageResources.Root;
        _integrateApplication = destinationRoot is null && packageRoot is null;
        Text = mode switch { "logs" => "Luminex - Logs", "packs" => "Luminex - Shader packs", "shortcuts" => "Luminex - Keyboard shortcuts", "uninstall" => "Luminex - Uninstall", _ => "Luminex Setup" };
        ClientSize = new Size(760, 620);
        MinimumSize = new Size(700, 590);
        StartPosition = FormStartPosition.CenterParent;
        AutoScaleMode = AutoScaleMode.Dpi;
        Font = new Font("Segoe UI", 10);
        BackColor = Color.FromArgb(24, 23, 35);
        ForeColor = Color.WhiteSmoke;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }
        var footer = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 66, FlowDirection = FlowDirection.RightToLeft, Padding = new Padding(16, 8, 16, 8), WrapContents = false };
        footer.Controls.AddRange([_close, _next, _back, _open]);
        _open.Visible = false;
        Controls.Add(_content); Controls.Add(footer); Controls.Add(_heading);
        _back.Click += (_, _) => { if (_page > 0) { _page--; ShowPage(); } };
        _close.Click += (_, _) => Close();
        _open.Click += (_, _) => { OpenManagerRequested = true; Close(); };
        _next.Click += async (_, _) => await Next();
        FormClosing += (_, e) => { if (_busy) { e.Cancel = true; MessageBox.Show(this, "Please wait for the current operation to finish.", "Luminex", MessageBoxButtons.OK, MessageBoxIcon.Information); } };
        FormClosed += (_, _) => { if (_integrateApplication && _mode == "uninstall" && _completed) Application.Exit(); };
        ReadShortcuts();
        _saveLogs.Checked = LogSettings.Read(_installRoot);
        if (_integrateApplication)
        {
            _startMenuShortcut.Checked = AppInstallation.ReadShortcutPreference();
            _desktopShortcut.Checked = AppInstallation.ReadDesktopShortcutPreference();
        }
        if (mode == "setup") _pages.Add(WelcomePage());
        if (mode == "logs") _pages.Add(LogsPage());
        if (mode is "setup" or "shortcuts") _pages.Add(ShortcutsPage());
        if (mode is "setup" or "packs") _pages.Add(PacksPage());
        if (mode == "uninstall") _pages.Add(UninstallPage());
        _pages.Add(ProgressPage());
        ShowPage();
    }

    private static Button Button(string text) => new SquareButton() { Text = text, Width = 142, Height = 40, FlatStyle = FlatStyle.Flat,
        BackColor = Color.FromArgb(45, 42, 59), ForeColor = Color.White, Margin = new Padding(5), Cursor = Cursors.Hand };
    private static FlowLayoutPanel Stack() => new() { Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true };
    private static Label Copy(string text, int height = 72) => new() { Text = text, Width = 590, Height = height, Margin = new Padding(0, 8, 0, 12) };
    private Control WelcomePage()
    {
        var page = Stack(); page.Tag = "Welcome to Luminex";
        page.Controls.Add(Copy("Let's get your shaders ready.\nChoose your shortcuts, pick your effects, and click Install.", 52));
        page.Controls.Add(Copy("Your existing presets and settings are kept when you update.\nYou can change shortcuts and add effects later in the manager.", 52));
        page.Controls.Add(Copy("Luminex needs a exploit that emulates Hyperion such as Volt.\nIt does not work with the normal Roblox client.", 52));
        page.Controls.Add(Copy("Install location:\n" + _installRoot, 56));
        page.Controls.Add(_saveLogs);
        page.Controls.Add(_startMenuShortcut);
        page.Controls.Add(_desktopShortcut);
        var licenses = Button("Third-party licenses"); licenses.Width = 190;
        licenses.Click += (_, _) => { try { PackageResources.ShowLicenses(this); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Could not open licenses"); } };
        page.Controls.Add(licenses);
        var about = Button("About saved logs"); about.Width = 180;
        about.Click += (_, _) => MessageBox.Show(this, LogSettings.Explanation, "About saved logs", MessageBoxButtons.OK, MessageBoxIcon.Information);
        page.Controls.Add(about);
        return page;
    }
    private Control LogsPage()
    {
        var page = Stack(); page.Tag = "Saved logs";
        page.Controls.Add(_saveLogs);
        page.Controls.Add(Copy(LogSettings.Explanation, 340));
        return page;
    }
    private void ReadShortcuts()
    {
        string path = Path.Combine(_installRoot, "Runtime", "ReShade.ini");
        if (!File.Exists(path)) return;
        string ini = File.ReadAllText(path);
        _menu = Shortcut.Parse(Regex.Match(ini, @"(?m)^KeyOverlay=(.*)\r?$").Groups[1].Value.Trim(), _menu);
        _effects = Shortcut.Parse(Regex.Match(ini, @"(?m)^KeyEffects=(.*)\r?$").Groups[1].Value.Trim(), _effects);
    }
    private Control ShortcutsPage()
    {
        var page = Stack(); page.Tag = "Choose your shortcuts";
        page.Controls.Add(Copy("Click a shortcut, then press the key you want.", 70));
        foreach (bool menu in new[] { true, false })
        {
            page.Controls.Add(Copy(menu ? "Open the ReShade menu" : "Turn all effects on or off", 28));
            var row = new FlowLayoutPanel { Width = 590, Height = 62, WrapContents = false };
            var choose = Button((menu ? _menu : _effects).Display); choose.Width = 350;
            choose.AccessibleName = menu ? "Menu shortcut" : "Effects shortcut";
            choose.Click += (_, _) =>
            {
                using var capture = new CaptureShortcutForm();
                if (capture.ShowDialog(this) != DialogResult.OK || capture.Binding == null) return;
                if (menu) _menu = capture.Binding; else _effects = capture.Binding;
                choose.Text = capture.Binding.Display;
            };
            var reset = Button(menu ? "Use Home" : "Clear shortcut");
            reset.Click += (_, _) => { if (menu) _menu = new((int)Keys.Home); else _effects = new(0); choose.Text = (menu ? _menu : _effects).Display; };
            row.Controls.AddRange([choose, reset]); page.Controls.Add(row);
        }
        page.Controls.Add(Copy("Tip: Home opens the menu by default. You can leave the effects shortcut unset.\nClose the client using Luminex before saving changes, so it cannot overwrite them later.", 90));
        return page;
    }
    private Control PacksPage()
    {
        var page = new Panel { Dock = DockStyle.Fill, Tag = "Choose your effects" };
        using var json = JsonDocument.Parse(File.ReadAllText(Path.Combine(_packageRoot, "installer", "ShaderPackages.json")));
        foreach (var p in json.RootElement.GetProperty("packages").EnumerateArray())
        {
            var pack = new Pack(p.GetProperty("id").GetString()!, p.GetProperty("name").GetString()!, p.GetProperty("description").GetString()!, p.GetProperty("repositoryUrl").GetString()!);
            _catalog.Add(pack); _packs.Items.Add(pack, pack.Id == "00");
        }
        var intro = new Label { Dock = DockStyle.Top, Height = 75, Text = "Only Standard effects are selected to get you started. All other packs are optional.\nDownloads come directly from the authors. Their licenses apply.\nYou can uncheck everything and add packs later." };
        var detail = new Label { Dock = DockStyle.Bottom, Height = 58, Text = "Select a pack to learn more. Shared Standard headers are added with any selection.", Padding = new Padding(0, 8, 0, 0) };
        var buttons = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 52, WrapContents = false };
        var all = Button("Select all"); var clear = Button("Unselect all"); var author = Button("Author page");
        all.Click += (_, _) => { for (int i = 0; i < _packs.Items.Count; i++) _packs.SetItemChecked(i, true); };
        clear.Click += (_, _) => { for (int i = 0; i < _packs.Items.Count; i++) _packs.SetItemChecked(i, false); };
        _packs.SelectedIndexChanged += (_, _) => { if (_packs.SelectedItem is Pack p) detail.Text = p.Description; };
        author.Click += (_, _) => { if (_packs.SelectedItem is Pack p && Uri.TryCreate(p.Url, UriKind.Absolute, out var uri) && uri.Scheme == "https")
            try { Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true }); } catch (Exception ex) { MessageBox.Show(this, ex.Message, "Could not open author page"); } };
        buttons.Controls.AddRange([all, clear, author]);
        page.Controls.Add(_packs); page.Controls.Add(detail); page.Controls.Add(buttons); page.Controls.Add(intro);
        return page;
    }
    private Control UninstallPage()
    {
        var page = Stack(); page.Tag = "Remove Luminex";
        page.Controls.Add(Copy("Luminex program files will be removed.\nYour settings, presets and shader packs are kept unless you select them below.", 80));
        foreach (var box in new[] { _settings, _shaders, _logs, _everything }) { box.Margin = new Padding(0, 12, 0, 12); page.Controls.Add(box); }
        _everything.CheckedChanged += (_, _) => { _settings.Enabled = _shaders.Enabled = _logs.Enabled = !_everything.Checked; };
        page.Controls.Add(Copy("Close any client using Luminex first.\nDeleted files do not go to the Recycle Bin. Back up anything you want to keep.", 80));
        return page;
    }
    private Control ProgressPage()
    {
        var page = new Panel { Dock = DockStyle.Fill, Tag = "Getting things ready" };
        var toggle = Button("Show details"); toggle.Dock = DockStyle.Bottom;
        toggle.Click += (_, _) => { _details.Visible = !_details.Visible; toggle.Text = _details.Visible ? "Hide details" : "Show details"; };
        page.Controls.Add(_details); page.Controls.Add(toggle); page.Controls.Add(_progress); page.Controls.Add(_status);
        return page;
    }
    private void ShowPage()
    {
        _content.Controls.Clear(); _content.Controls.Add(_pages[_page]);
        _heading.Text = (string)_pages[_page].Tag!;
        _back.Visible = _page > 0 && _page < _pages.Count - 1;
        _next.Text = _page == _pages.Count - 2 ? _mode switch { "logs" => "Save", "uninstall" => "Uninstall", "shortcuts" => "Save shortcuts", "packs" => "Download", _ => "Install" } : "Next";
    }
    private async Task Next()
    {
        if (_busy || _completed) return;
        if (_mode is "setup" or "shortcuts" && _menu.Key != 0 && _menu == _effects)
        { MessageBox.Show(this, "Choose different shortcuts for the menu and effects.", "Shortcuts overlap", MessageBoxButtons.OK, MessageBoxIcon.Information); return; }
        if (_page < _pages.Count - 2) { _page++; ShowPage(); return; }
        if (_mode == "uninstall" && _page != _pages.Count - 1)
        {
            string data = _everything.Checked ? "The following will be deleted:\n\n" +
                "- Program files and manager shortcuts\n" +
                "- Settings and presets\n" +
                "- Shader packs and textures\n" +
                "- Saved logs\n" +
                "- Backups\n" +
                "- Any custom files in the installation folder\n\n" +
                "The entire installation folder will be removed. Deleted files will not go to the Recycle Bin." :
                "Program files will be removed.\n" + string.Join("\n", new[] { _settings.Checked ? "Settings and presets will be deleted." : "Settings and presets will be kept.",
                _shaders.Checked ? "Shader packs will be deleted." : "Shader packs will be kept.", _logs.Checked ? "Logs will be deleted." : "Logs will be kept." });
            if (MessageBox.Show(this, data + "\n\nContinue?", "Uninstall Luminex", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes) return;
        }
        _page = _pages.Count - 1; ShowPage();
        _busy = true; _next.Enabled = _close.Enabled = false; _back.Visible = _open.Visible = false;
        _progress.Style = ProgressBarStyle.Marquee; _details.Clear();
        try
        {
            if (_mode == "setup")
            {
                Status("Installing Luminex", "Checking the package and installing program files...");
                await RunScript("Install-Package.ps1");
                if (_integrateApplication) AppInstallation.Register(createStartMenuShortcut: _startMenuShortcut.Checked, createDesktopShortcut: _desktopShortcut.Checked);
                _baseInstalled = true;
            }
            if (_mode is "setup" or "logs") LogSettings.Save(_installRoot, _saveLogs.Checked);
            if (_mode is "setup" or "shortcuts")
            {
                Status("Saving your shortcuts", $"Menu: {_menu.Display}\nEffects: {_effects.Display}");
                await RunScript("Set-Keybinds.ps1", "-Effects", _effects.Value, "-Overlay", _menu.Value);
            }
            if (_mode is "setup" or "packs" && _packs.CheckedItems.Count > 0)
            {
                Status("Downloading effects", "Keep this window open while your selected packs download.");
                await RunScript("Install-ShaderPacks.ps1", "-PackageIds", string.Join(',', _packs.CheckedItems.Cast<Pack>().Select(p => p.Id)));
            }
            if (_mode == "uninstall")
            {
                Status("Removing Luminex", "Removing only the items you selected...");
                if (_integrateApplication) AppInstallation.CheckOtherManagers();
                var args = new List<string>();
                if (_everything.Checked) args.Add("-RemoveAll");
                else { if (_settings.Checked) args.Add("-RemoveSettings"); if (_shaders.Checked) args.Add("-RemoveShaders"); if (_logs.Checked) args.Add("-RemoveLogs"); }
                await RunScript("Uninstall-Package.ps1", args.ToArray());
                if (_integrateApplication) AppInstallation.ScheduleRemoval();
            }
            _completed = true; _next.Visible = false;
            Status(_mode == "uninstall" ? "Finish uninstalling" : "You're all set", _mode switch
            {
                "setup" => $"Open the manager, select your Roblox window, and click Inject Into Roblox.\nPress {_menu.Display} in the game to open ReShade.",
                "shortcuts" => $"Menu: {_menu.Display}\nEffects: {_effects.Display}",
                "logs" => "Log preference saved. Close the client and load Luminex into a new session for it to take effect. Existing logs have been kept.",
                "packs" => _packs.CheckedItems.Count == 0 ? "No packs were selected. You can add effects here whenever you like." : "Your effects are ready. If ReShade is open, press Reload to see them.",
                _ => "Selected items have been removed. Close this window to finish removing the manager. If removal fails, a message will explain how to retry."
            });
            _open.Visible = _mode == "setup"; _close.Text = "Close";
        }
        catch (Exception ex)
        {
            Status(_baseInstalled ? "Installed, but a step needs attention" : "Couldn't finish", _baseInstalled
                ? "Luminex is installed. Check your connection if downloads failed, then click Retry.\nYou can also open the manager and add packs later."
                : "No need to start over. Close any client using Luminex, check the details below, and click Retry.");
            _details.AppendText(Environment.NewLine + ex.Message);
            _next.Text = "Retry"; _next.Enabled = true; _open.Visible = _baseInstalled; _back.Visible = true;
        }
        finally { _busy = false; _close.Enabled = true; _progress.Style = ProgressBarStyle.Blocks; _progress.Value = _completed ? 100 : 0; }
    }
    private void Status(string title, string message) { _heading.Text = title; _status.Text = message; }
    protected override void Dispose(bool disposing)
    {
        if (disposing) foreach (var page in _pages) page.Dispose();
        base.Dispose(disposing);
    }
    private async Task RunScript(string file, params string[] arguments)
    {
        string script = Path.Combine(_packageRoot, "installer", file);
        if (!File.Exists(script)) throw new FileNotFoundException("Installation resources are missing. Download LuminexSetup.exe again. Missing " + file);
        var info = new ProcessStartInfo(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"))
        { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = _packageRoot };
        foreach (string arg in new[] { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-DestinationRoot", _installRoot }.Concat(arguments)) info.ArgumentList.Add(arg);
        using var process = Process.Start(info) ?? throw new InvalidOperationException("Could not start the installation helper.");
        async Task Read(StreamReader reader)
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                _details.AppendText(line + Environment.NewLine);
                var match = Regex.Match(line, @"^\[(\d+)/(\d+)\] Downloading (.*?) from https://");
                if (match.Success) _status.Text = $"Downloading {match.Groups[3].Value}\nPack {match.Groups[1].Value} of {match.Groups[2].Value}. Please keep this window open.";
            }
        }
        await Task.WhenAll(Read(process.StandardOutput), Read(process.StandardError), process.WaitForExitAsync());
        if (process.ExitCode != 0) throw new InvalidOperationException("This step did not finish. See the details above, then retry.");
    }
}
