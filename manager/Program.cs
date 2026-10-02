using System.Diagnostics;
using System.Security.Principal;

namespace LuminexManager;

internal static class Program
{
    [STAThread]
    private static void Main(string[] args)
    {
        ApplicationConfiguration.Initialize();
        try
        {
            string mode = args.FirstOrDefault()?.TrimStart('-') ??
                (Environment.ProcessPath?.StartsWith(AppInstallation.AppRoot + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) == true ? "manager" : "setup");
            if (mode == "licenses") { PackageResources.ShowLicenses(); return; }
            if (mode == "manager" && !File.Exists(Path.Combine(SetupForm.InstallRoot, "Runtime", "LuminexRuntime.dll"))) mode = "setup";
            if (mode is "setup" or "packs" or "shortcuts" or "uninstall" or "logs")
            {
                using var setup = new SetupForm(mode);
                setup.ShowDialog();
                if (!setup.OpenManagerRequested) return;
                if (AppInstallation.InstalledExecutable is { } installed)
                {
                    Process.Start(new ProcessStartInfo(installed, "--manager") { UseShellExecute = true });
                    return;
                }
            }
            Application.Run(new MainForm());
        }
        catch (Exception ex)
        {
            MessageBox.Show("Luminex could not open. Run LuminexSetup.exe again to repair the installation.\n\n" + ex.Message,
                "Luminex", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally { PackageResources.Cleanup(); }
    }
}

internal sealed class MainForm : Form
{
    private readonly bool _isAdministrator = IsRunningAsAdministrator();

    private static bool IsRunningAsAdministrator()
    {
        using var identity = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(identity).IsInRole(WindowsBuiltInRole.Administrator);
    }

    private static readonly Color Background = Color.FromArgb(13, 13, 20);
    private static readonly Color Card = Color.FromArgb(24, 23, 35);
    private static readonly Color CardLight = Color.FromArgb(32, 30, 46);
    private static readonly Color Purple = Color.FromArgb(15, 112, 238);
    private static readonly Color PurpleLight = Color.FromArgb(70, 174, 255);
    private static readonly Color Muted = Color.FromArgb(164, 160, 181);
    private static readonly Color Good = Color.FromArgb(74, 222, 128);
    private static readonly Color Warning = Color.FromArgb(251, 191, 36);

    private readonly Label _stateValue = new();
    private readonly Label _targetValue = new();
    private readonly Label _runtimeValue = new();
    private readonly Label _effectsValue = new();
    private readonly Label _notice = new();
    private readonly TextBox _activity = new();
    private readonly ComboBox _clientSelector = new();
    private readonly Button _launchButton;
    private readonly System.Windows.Forms.Timer _statusTimer = new() { Interval = 1000 };
    private readonly System.Windows.Forms.Timer _pickTimer = new() { Interval = 15 };
    private bool _pickArmed, _pickClicked;
    private DateTime _pickDeadline;
    private nint _pickedWindow;
    private uint _pickedPid;
    private int[] _knownClientIds = [];

    public MainForm()
    {
        Text = "Luminex Shaders Manager";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(900, 730);
        MinimumSize = new Size(820, 690);
        AutoScaleMode = AutoScaleMode.Dpi;
        BackColor = Background;
        ForeColor = Color.White;
        Font = new Font("Segoe UI", 10F);
        DoubleBuffered = true;
        try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch { }

        var header = new Panel { Dock = DockStyle.Top, Height = 108, Padding = new Padding(34, 18, 34, 14) };
        var brandImage = LoadBrandMark();
        Control mark = brandImage is null
            ? new Label
            {
                Text = "LX",
                AutoSize = false,
                TextAlign = ContentAlignment.MiddleCenter,
                Font = new Font("Segoe UI", 22, FontStyle.Bold),
                BackColor = Purple,
                ForeColor = Color.White
            }
            : new PictureBox
            {
                Image = brandImage,
                SizeMode = PictureBoxSizeMode.Zoom,
                BackColor = Color.Transparent
            };
        mark.Size = new Size(72, 72);
        mark.Location = new Point(30, 17);
        mark.AccessibleName = "Luminex logo";
        header.Controls.Add(mark);
        header.Controls.Add(new Label
        {
            Text = "Luminex Shaders",
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 21F, FontStyle.Bold),
            Location = new Point(116, 24)
        });
        header.Controls.Add(new Label
        {
            Text = "LUMINEX MANAGER  •  ALPHA",
            AutoSize = true,
            Font = new Font("Segoe UI Semibold", 8.5F),
            ForeColor = PurpleLight,
            Location = new Point(119, 66)
        });
        var body = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            Padding = new Padding(34, 10, 34, 28),
            ColumnCount = 2,
            RowCount = 1
        };
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 57));
        body.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 43));
        Controls.Add(body);
        Controls.Add(header);

        var menu = new MenuStrip { BackColor = Card, ForeColor = Color.White };
        foreach (var (label, mode) in new[] { ("Install / repair", "setup"), ("Keyboard shortcuts", "shortcuts"), ("Shader packs", "packs"), ("Logs", "logs"), ("Uninstall", "uninstall") })
        {
            var item = new ToolStripMenuItem(label);
            item.Click += (_, _) =>
            {
                _statusTimer.Stop();
                try { using var setup = new SetupForm(mode); setup.ShowDialog(this); }
                catch (Exception ex) { MessageBox.Show(this, "Run LuminexSetup.exe again to repair the installation.\n\n" + ex.Message, "Could not open", MessageBoxButtons.OK, MessageBoxIcon.Error); }
                finally { if (!IsDisposed) { RefreshStatus(); _statusTimer.Start(); } }
            };
            menu.Items.Add(item);
        }
        var about = new ToolStripMenuItem("About");
        var licenses = new ToolStripMenuItem("Third-party licenses");
        licenses.Click += (_, _) =>
        {
            try { PackageResources.ShowLicenses(this); }
            catch (Exception ex) { MessageBox.Show(this, ex.Message, "Could not open licenses"); }
        };
        about.DropDownItems.Add(licenses);
        menu.Items.Add(about);
        MainMenuStrip = menu;
        Controls.Add(menu);

        var statusCard = MakeCard(new Padding(26));
        body.Controls.Add(statusCard, 0, 0);
        body.SetCellPosition(statusCard, new TableLayoutPanelCellPosition(0, 0));
        statusCard.Margin = new Padding(0, 0, 12, 0);

        var statusLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 6,
            BackColor = Color.Transparent,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        statusLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 36));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 48));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 150));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 68));
        statusLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 82));
        statusCard.Controls.Add(statusLayout);

        statusLayout.Controls.Add(new Label
        {
            Text = "Installation status",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Font = new Font("Segoe UI Semibold", 15F, FontStyle.Bold)
        }, 0, 0);
        _stateValue.Dock = DockStyle.Fill;
        _stateValue.TextAlign = ContentAlignment.MiddleLeft;
        _stateValue.Margin = Padding.Empty;
        _stateValue.Font = new Font("Segoe UI Semibold", 19F, FontStyle.Bold);
        _stateValue.ForeColor = Muted;
        statusLayout.Controls.Add(_stateValue, 0, 1);

        var details = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 3, Padding = new Padding(0, 10, 0, 0), Margin = Padding.Empty };
        details.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 44));
        details.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 56));
        AddDetail(details, 0, "Target process", _targetValue);
        AddDetail(details, 1, "Runtime", _runtimeValue);
        AddDetail(details, 2, "Effects found", _effectsValue);
        statusLayout.Controls.Add(details, 0, 2);

        _notice.Dock = DockStyle.Fill;
        _notice.ForeColor = Muted;
        _notice.Padding = new Padding(0, 8, 0, 0);
        _notice.TextAlign = ContentAlignment.TopLeft;
        statusLayout.Controls.Add(_notice, 0, 3);

        var defenderButton = MakeButton("Add to Windows Defender exclusions", CardLight);
        defenderButton.Dock = DockStyle.Fill;
        defenderButton.Click += async (_, _) => await AddDefenderExclusion(defenderButton);
        statusLayout.Controls.Add(defenderButton, 0, 4);
        statusLayout.Controls.Add(new Label
        {
            Text = "Recommended to prevent Windows Defender from deleting important files. May not work with other Anti-Virus software.",
            Dock = DockStyle.Fill,
            ForeColor = Muted,
            Font = new Font("Segoe UI", 9F),
            Margin = Padding.Empty
        }, 0, 5);

        var actionCard = MakeCard(new Padding(24));
        actionCard.Margin = new Padding(12, 0, 0, 0);
        body.Controls.Add(actionCard, 1, 0);
        var actionLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            BackColor = Color.Transparent,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        actionLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        actionLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 42));
        actionLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, _isAdministrator ? 176 : 224));
        actionLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        actionLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 58));
        actionLayout.RowStyles.Add(new RowStyle(SizeType.Percent, 100));
        actionCard.Controls.Add(actionLayout);

        actionLayout.Controls.Add(new Label
        {
            Text = "Manage Luminex",
            Dock = DockStyle.Fill,
            TextAlign = ContentAlignment.MiddleLeft,
            Font = new Font("Segoe UI Semibold", 15F, FontStyle.Bold)
        }, 0, 0);

        var selectorLayout = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 5,
            BackColor = Color.Transparent,
            Margin = Padding.Empty,
            Padding = Padding.Empty
        };
        selectorLayout.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        selectorLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 24));
        selectorLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 40));
        selectorLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 46));
        selectorLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, 66));
        selectorLayout.RowStyles.Add(new RowStyle(SizeType.Absolute, _isAdministrator ? 0 : 48));
        selectorLayout.Controls.Add(new Label
        {
            Text = "Target Roblox PID",
            Dock = DockStyle.Fill,
            ForeColor = Muted,
            TextAlign = ContentAlignment.MiddleLeft,
            Font = new Font("Segoe UI Semibold", 9F, FontStyle.Bold)
        }, 0, 0);
        _clientSelector.Dock = DockStyle.Fill;
        _clientSelector.DropDownStyle = ComboBoxStyle.DropDownList;
        _clientSelector.FlatStyle = FlatStyle.Flat;
        _clientSelector.BackColor = CardLight;
        _clientSelector.ForeColor = Color.White;
        _clientSelector.Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold);
        _clientSelector.Margin = new Padding(0, 0, 0, 6);
        selectorLayout.Controls.Add(_clientSelector, 0, 1);
        var pickButton = MakeButton("Select Roblox window", CardLight);
        pickButton.Click += (_, _) => BeginWindowPick();
        selectorLayout.Controls.Add(pickButton, 0, 2);
        selectorLayout.Controls.Add(new Label
        {
            Text = "Select a window, then click the Roblox title bar for your account. This identifies its PID; account names are not detected. Esc cancels.",
            Dock = DockStyle.Fill,
            ForeColor = Muted,
            TextAlign = ContentAlignment.TopLeft,
            Font = new Font("Segoe UI", 8.25F),
            Padding = new Padding(0, 3, 0, 0)
        }, 0, 3);
        selectorLayout.Controls.Add(new Label
        {
            Text = "Its recommended to run Luminex Shaders Manager as administrator",
            Visible = !_isAdministrator,
            Dock = DockStyle.Fill,
            ForeColor = Warning,
            Font = new Font("Segoe UI", 8.25F),
            Padding = new Padding(0, 3, 0, 0)
        }, 0, 4);
        actionLayout.Controls.Add(selectorLayout, 0, 1);

        _launchButton = MakeButton("Inject Into Roblox", Color.FromArgb(45, 42, 59));
        _launchButton.Click += async (_, _) => await LaunchNow();
        _launchButton.Dock = DockStyle.Fill;
        actionLayout.Controls.Add(_launchButton, 0, 2);

        var folderButton = MakeButton("Open configs folder", Color.FromArgb(45, 42, 59));
        folderButton.Click += (_, _) => OpenRuntimeFolder();
        folderButton.Dock = DockStyle.Fill;
        actionLayout.Controls.Add(folderButton, 0, 3);

        _activity.Multiline = true;
        _activity.ReadOnly = true;
        _activity.BorderStyle = BorderStyle.None;
        _activity.BackColor = Color.FromArgb(17, 16, 25);
        _activity.ForeColor = Muted;
        _activity.Font = new Font("Cascadia Mono", 8.5F);
        _activity.Dock = DockStyle.Fill;
        _activity.ScrollBars = ScrollBars.Vertical;
        _activity.Margin = new Padding(0, 10, 0, 0);
        actionLayout.Controls.Add(_activity, 0, 4);

        Shown += (_, _) =>
        {
            RefreshStatus();
            _statusTimer.Start();
        };
        _statusTimer.Tick += (_, _) => RefreshStatus();
        _pickTimer.Tick += (_, _) => PollWindowPick();
        FormClosed += (_, _) => { _statusTimer.Dispose(); _pickTimer.Dispose(); };
    }

    private static Panel MakeCard(Padding padding)
    {
        var panel = new Panel { Dock = DockStyle.Fill, BackColor = Card, Padding = padding };
        panel.Paint += (_, e) =>
        {
            using var pen = new Pen(CardLight);
            e.Graphics.DrawRectangle(pen, 0, 0, panel.ClientSize.Width - 1, panel.ClientSize.Height - 1);
        };
        return panel;
    }

    private static Button MakeButton(string text, Color background) => new SquareButton()
    {
        Text = text,
        Dock = DockStyle.Top,
        Height = 45,
        Margin = new Padding(0, 0, 0, 10),
        FlatStyle = FlatStyle.Flat,
        BackColor = background,
        ForeColor = Color.White,
        Font = new Font("Segoe UI Semibold", 10F, FontStyle.Bold),
        Cursor = Cursors.Hand,
        FlatAppearance = { BorderSize = 0 }
    };

    private static void AddDetail(TableLayoutPanel panel, int row, string name, Label value)
    {
        panel.RowStyles.Add(new RowStyle(SizeType.Percent, 100F / panel.RowCount));
        panel.Controls.Add(new Label { Text = name, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, ForeColor = Muted }, 0, row);
        value.Dock = DockStyle.Fill;
        value.TextAlign = ContentAlignment.MiddleLeft;
        value.Font = new Font("Segoe UI Semibold", 9.5F, FontStyle.Bold);
        panel.Controls.Add(value, 1, row);
    }

    private void RefreshStatus()
    {
        var local = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);
        var runtime = Path.Combine(local, "LuminexShaders", "Runtime", "LuminexRuntime.dll");
        var reshade = Path.Combine(local, "LuminexShaders", "Runtime", "LuminexReShadeRuntime.dll");
        var launcher = Path.Combine(local, "LuminexShaders", "Launcher", "LuminexLoader.exe");
        bool runtimeInstalled = File.Exists(runtime) && File.Exists(reshade);
        bool launcherInstalled = File.Exists(launcher);
        bool complete = runtimeInstalled && launcherInstalled;
        bool partial = runtimeInstalled || launcherInstalled;

        _stateValue.Text = complete ? "Installed" : partial ? "Needs repair" : "Not installed";
        _stateValue.ForeColor = complete ? Good : partial ? Warning : Muted;
        _runtimeValue.Text = runtimeInstalled ? "Ready" : "Missing";
        _runtimeValue.ForeColor = runtimeInstalled ? Good : Warning;
        string effectsRoot = Path.Combine(local, "LuminexShaders", "Runtime", "ReShade", "Shaders");
        int count = Directory.Exists(effectsRoot) ? Directory.EnumerateFiles(effectsRoot, "*.fx", SearchOption.AllDirectories).Count() : 0;
        _effectsValue.Text = count == 0 ? "None installed" : $"{count} shader files";
        _effectsValue.ForeColor = count > 0 ? Color.White : Muted;

        int[] clientIds = GetRobloxClientIds();
        RefreshClientSelector(clientIds);
        bool robloxRunning = clientIds.Length > 0;
        _targetValue.Text = clientIds.Length switch
        {
            0 => "No Roblox clients running",
            1 => $"1 client running | PID {clientIds[0]}",
            _ => $"{clientIds.Length} Roblox clients running"
        };
        _targetValue.ForeColor = robloxRunning ? Good : Muted;
        _notice.Text = !complete
            ? "Click Install / repair above to set up Luminex."
            : clientIds.Length > 1
                ? "Choose the Roblox PID you want, then load Luminex into only that client."
                : robloxRunning
                ? "Roblox detected. Click Inject Into Roblox."
                : "Start Roblox, then return here to load Luminex.";
        _notice.ForeColor = !complete ? Warning : robloxRunning ? Good : Muted;
        _launchButton.Enabled = complete && _clientSelector.SelectedItem is RobloxClientChoice;
    }

    private static int[] GetRobloxClientIds()
    {
        Process[] processes = Process.GetProcessesByName("RobloxPlayerBeta");
        try
        {
            return processes.Select(process => process.Id).Order().ToArray();
        }
        finally
        {
            foreach (Process process in processes)
                process.Dispose();
        }
    }

    private void RefreshClientSelector(int[] clientIds)
    {
        if (_knownClientIds.SequenceEqual(clientIds))
            return;

        int? previousPid = (_clientSelector.SelectedItem as RobloxClientChoice)?.Pid;
        _clientSelector.BeginUpdate();
        try
        {
            _clientSelector.Items.Clear();
            foreach (int pid in clientIds)
                _clientSelector.Items.Add(new RobloxClientChoice(pid));

            int previousIndex = previousPid.HasValue ? Array.IndexOf(clientIds, previousPid.Value) : -1;
            _clientSelector.SelectedIndex = previousIndex >= 0 ? previousIndex : clientIds.Length > 0 ? 0 : -1;
            _knownClientIds = clientIds;
        }
        finally
        {
            _clientSelector.EndUpdate();
        }
    }

    private void BeginWindowPick()
    {
        _pickArmed = _pickClicked = false;
        _pickedWindow = 0;
        _pickedPid = 0;
        _pickDeadline = DateTime.UtcNow.AddSeconds(30);
        _statusTimer.Stop();
        Hide();
        _pickTimer.Start();
    }

    private void PollWindowPick()
    {
        if (WindowPicker.Down(0x1B) || DateTime.UtcNow >= _pickDeadline)
        {
            FinishWindowPick(false);
            return;
        }
        bool down = WindowPicker.Down(0x01);
        if (!_pickArmed) { if (!down) _pickArmed = true; return; }
        if (!_pickClicked && down)
        {
            _pickedWindow = WindowPicker.WindowUnderCursor();
            WindowPicker.GetWindowThreadProcessId(_pickedWindow, out _pickedPid);
            _pickClicked = true;
        }
        if (_pickClicked && !down) FinishWindowPick(true);
    }

    private void FinishWindowPick(bool selected)
    {
        _pickTimer.Stop();
        Show();
        Activate();
        try
        {
            RefreshStatus();
            if (!selected) { AppendActivity("> Window selection cancelled or timed out."); return; }
            int pid = WindowPicker.ReadRobloxPid(_pickedWindow);
            if (pid != _pickedPid) throw new InvalidOperationException("That window changed. Select it again.");
            var choice = _clientSelector.Items.Cast<RobloxClientChoice>().FirstOrDefault(item => item.Pid == pid)
                ?? throw new InvalidOperationException("That Roblox client has closed. Select another window.");
            _clientSelector.SelectedItem = choice;
            AppendActivity($"> Selected Roblox PID {pid} from its window. Click Inject Into Roblox when ready.");
        }
        catch (Exception ex) { AppendActivity($"> {ex.Message}"); }
        finally { _statusTimer.Start(); }
    }

    private async Task LaunchNow()
    {
        try
        {
            if (_clientSelector.SelectedItem is not RobloxClientChoice selected)
                throw new InvalidOperationException("Select a running Roblox PID first.");

            using Process target = Process.GetProcessById(selected.Pid);
            if (!target.ProcessName.Equals("RobloxPlayerBeta", StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException($"PID {selected.Pid} is no longer a Roblox client.");

            string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LuminexShaders");
            string loader = Path.Combine(root, "Launcher", "LuminexLoader.exe");
            string runtime = Path.Combine(root, "Runtime", "LuminexRuntime.dll");
            var startInfo = new ProcessStartInfo
            {
                FileName = loader,
                WorkingDirectory = Path.GetDirectoryName(loader)!,
                UseShellExecute = true,
                Verb = _isAdministrator ? "open" : "runas",
                WindowStyle = ProcessWindowStyle.Hidden
            };
            startInfo.ArgumentList.Add(selected.Pid.ToString());
            startInfo.ArgumentList.Add(runtime);
            AppendActivity(_isAdministrator
                ? $"> Loading Luminex into Roblox PID {selected.Pid}."
                : $"> Loading Luminex into Roblox PID {selected.Pid}; accept the UAC prompt.");
            using Process loaderProcess = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start loader.");
            await loaderProcess.WaitForExitAsync();
            AppendActivity(loaderProcess.ExitCode == 0
                ? "> Luminex loaded. Press your menu shortcut in Roblox to open ReShade."
                : "> Luminex could not load. Check that this client supports Luminex, then try again. Details are in the runtime logs.");
        }
        catch (Exception ex) { MessageBox.Show(this, ex.Message, "Launch failed", MessageBoxButtons.OK, MessageBoxIcon.Error); }
    }

    private async Task AddDefenderExclusion(Button button)
    {
        string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LuminexShaders");
        if (!Directory.Exists(root))
        {
            MessageBox.Show(this, "Install Luminex Shaders first.", "Windows Defender exclusions", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        if (MessageBox.Show(this,
            $"Add this folder to Windows Defender exclusions?\n\n{root}\n\nWindows Defender will skip scanning this folder. This is optional and may not work with other antivirus software. Administrator permission is required.",
            "Windows Defender exclusions", MessageBoxButtons.YesNo, MessageBoxIcon.Warning, MessageBoxDefaultButton.Button2) != DialogResult.Yes)
            return;

        button.Enabled = false;
        try
        {
            // Pass the original user's exact install path even if UAC uses another administrator account.
            string literal = root.Replace("'", "''");
            string script = $"$ErrorActionPreference = 'Stop'; try {{ $path = '{literal}'; Add-MpPreference -ExclusionPath $path; if (@((Get-MpPreference).ExclusionPath) -notcontains $path) {{ exit 2 }}; exit 0 }} catch {{ exit 1 }}";
            var startInfo = new ProcessStartInfo
            {
                FileName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe"),
                UseShellExecute = true,
                Verb = _isAdministrator ? "open" : "runas",
                WindowStyle = ProcessWindowStyle.Hidden
            };
            startInfo.ArgumentList.Add("-NoProfile");
            startInfo.ArgumentList.Add("-NonInteractive");
            startInfo.ArgumentList.Add("-EncodedCommand");
            startInfo.ArgumentList.Add(Convert.ToBase64String(System.Text.Encoding.Unicode.GetBytes(script)));
            using Process process = Process.Start(startInfo) ?? throw new InvalidOperationException("Could not start Windows Defender configuration.");
            await process.WaitForExitAsync();
            if (process.ExitCode != 0)
                throw new InvalidOperationException("Windows Defender could not confirm the exclusion. It may be disabled, managed by your organization, or replaced by another antivirus. Check Windows Security for the current exclusion settings.");

            AppendActivity($"> Windows Defender exclusion confirmed: {root}");
            MessageBox.Show(this, $"Added to Windows Defender exclusions:\n\n{root}\n\nYou can remove it in Windows Security > Virus & threat protection > Manage settings > Exclusions.",
                "Exclusion added", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
        catch (System.ComponentModel.Win32Exception ex) when (ex.NativeErrorCode == 1223)
        {
            AppendActivity("> Windows Defender exclusion cancelled.");
        }
        catch (Exception ex)
        {
            AppendActivity($"> {ex.Message}");
            MessageBox.Show(this, ex.Message, "Exclusion not confirmed", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
        finally { button.Enabled = true; }
    }

    private sealed record RobloxClientChoice(int Pid)
    {
        public override string ToString() => $"PID {Pid}";
    }

    private static void OpenRuntimeFolder()
    {
        string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "LuminexShaders", "Runtime");
        Directory.CreateDirectory(path);
        Process.Start(new ProcessStartInfo { FileName = "explorer.exe", Arguments = $"\"{path}\"", UseShellExecute = true });
    }

    private void AppendActivity(string text)
    {
        if (_activity.TextLength > 0) _activity.AppendText(Environment.NewLine);
        _activity.AppendText(text.Replace("\r\n", "\n").Replace("\n", Environment.NewLine));
    }

    private static Image? LoadBrandMark()
    {
        using Stream? stream = typeof(MainForm).Assembly.GetManifestResourceStream("LuminexMark.png");
        if (stream is null) return null;
        using Image source = Image.FromStream(stream);
        return new Bitmap(source);
    }
}
