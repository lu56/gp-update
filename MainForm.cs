using System.Drawing;
using System.Windows.Forms;
using Microsoft.Win32;
using System.Diagnostics;

namespace GatewayPolicy;

public class MainForm : Form
{
    private readonly AppConfig _config;
    private readonly RouteGuard _guard;

    // Controls
    private TabControl _tabControl = null!;
    private NotifyIcon _notifyIcon = null!;
    private ContextMenuStrip _trayMenu = null!;
    private Icon _iconGreen = null!;
    private Icon _iconYellow = null!;
    private Icon _iconRed = null!;

    // Status tab
    private Label _lblState = null!;
    private Label _lblMainNic = null!;
    private Label _lblTapNic = null!;
    private Label _lblLastFix = null!;
    private Label _lblTotalFixes = null!;
    private Button _btnToggle = null!;
    private Button _btnFixNow = null!;

    // Settings tab
    private NumericUpDown _numInterval = null!;
    private NumericUpDown _numTapMetric = null!;
    private NumericUpDown _numMainMetric = null!;
    private CheckBox _chkAutoStart = null!;
    private CheckBox _chkMinimizeTray = null!;
    private CheckBox _chkNotify = null!;
    private TextBox _txtBarkServer = null!;
    private TextBox _txtBarkKey = null!;
    private CheckBox _chkBark = null!;
    private DataGridView _dgCustomRoutes = null!;
    private ListBox _lbPrivateNets = null!;

    // Log tab
    private TextBox _txtLog = null!;
    private Button _btnExportLog = null!;
    private Button _btnClearLog = null!;

    public MainForm(AppConfig config, RouteGuard guard)
    {
        _config = config;
        _guard = guard;

        InitializeIcons();
        InitializeTray();
        InitializeForm();
        BuildStatusTab();
        BuildSettingsTab();
        BuildLogTab();
        BuildAboutTab();

        // Wire events
        _guard.OnLog += OnGuardLog;
        _guard.OnStateChanged += OnGuardStateChanged;
        _guard.OnFixCompleted += OnGuardFixCompleted;

        // Load current state
        RefreshStatus();

        // Auto check update after 5 seconds (silent)
        if (_config.AutoUpdateCheck && !string.IsNullOrEmpty(_config.UpdateRepo))
        {
            var _ = Task.Run(async () =>
            {
                await Task.Delay(5000);
                try { this.Invoke(async () => await CheckUpdate(silent: true)); } catch { }
            });
        }
    }

    private void InitializeIcons()
    {
        // Create colored icons programmatically
        _iconGreen = CreateColorIcon(Color.Green);
        _iconYellow = CreateColorIcon(Color.Gold);
        _iconRed = CreateColorIcon(Color.Red);
    }

    private static Icon CreateColorIcon(Color color)
    {
        using var bmp = new Bitmap(16, 16);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.Transparent);
        using var brush = new SolidBrush(color);
        g.FillEllipse(brush, 1, 1, 14, 14);
        g.DrawEllipse(Pens.White, 1, 1, 14, 14);
        return Icon.FromHandle(bmp.GetHicon());
    }

    private void InitializeTray()
    {
        _trayMenu = new ContextMenuStrip();
        _trayMenu.Items.Add("状态: 运行中", null, (s, e) => ShowWindow());
        _trayMenu.Items.Add("-");
        _trayMenu.Items.Add("立即修复", null, (s, e) => FixNow());
        _trayMenu.Items.Add("暂停 / 恢复", null, (s, e) => ToggleMonitor());
        _trayMenu.Items.Add("-");
        _trayMenu.Items.Add("打开窗口", null, (s, e) => ShowWindow());
        _trayMenu.Items.Add("退出", null, (s, e) => ExitApp());

        _notifyIcon = new NotifyIcon
        {
            Icon = _iconGreen,
            Text = "GatewayPolicy",
            Visible = true,
            ContextMenuStrip = _trayMenu
        };
        _notifyIcon.DoubleClick += (s, e) => ShowWindow();
    }

    private void InitializeForm()
    {
        Text = "GatewayPolicy";
        Size = new Size(600, 500);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedSingle;
        MaximizeBox = false;
        MinimumSize = new Size(500, 400);

        _tabControl = new TabControl { Dock = DockStyle.Fill, Padding = new Point(12, 6) };
        Controls.Add(_tabControl);
    }

    // ===== Status Tab =====

    private void BuildStatusTab()
    {
        var tab = new TabPage("状态");
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 2, RowCount = 7, Padding = new Padding(10) };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        _lblState = CreateValueLabel("已停止");
        _lblMainNic = CreateValueLabel("-");
        _lblTapNic = CreateValueLabel("-");
        _lblLastFix = CreateValueLabel("-");
        _lblTotalFixes = CreateValueLabel("0");

        panel.Controls.Add(CreateFieldLabel("监控状态:"), 0, 0);
        panel.Controls.Add(_lblState, 1, 0);
        panel.Controls.Add(CreateFieldLabel("主网卡:"), 0, 1);
        panel.Controls.Add(_lblMainNic, 1, 1);
        panel.Controls.Add(CreateFieldLabel("TAP 适配器:"), 0, 2);
        panel.Controls.Add(_lblTapNic, 1, 2);
        panel.Controls.Add(CreateFieldLabel("上次修复:"), 0, 3);
        panel.Controls.Add(_lblLastFix, 1, 3);
        panel.Controls.Add(CreateFieldLabel("累计修复:"), 0, 4);
        panel.Controls.Add(_lblTotalFixes, 1, 4);

        var btnPanel = new FlowLayoutPanel { Dock = DockStyle.Fill, FlowDirection = FlowDirection.LeftToRight };
        _btnToggle = new Button { Text = "启动监控", Size = new Size(130, 32), Margin = new Padding(0, 10, 10, 0) };
        _btnToggle.Click += (s, e) => ToggleMonitor();
        _btnFixNow = new Button { Text = "立即修复", Size = new Size(100, 32), Margin = new Padding(0, 10, 0, 0) };
        _btnFixNow.Click += (s, e) => FixNow();

        btnPanel.Controls.Add(_btnToggle);
        btnPanel.Controls.Add(_btnFixNow);

        panel.Controls.Add(btnPanel, 0, 5);
        panel.SetColumnSpan(btnPanel, 2);

        tab.Controls.Add(panel);
        _tabControl.TabPages.Add(tab);
    }

    // ===== Settings Tab =====

    private void BuildSettingsTab()
    {
        var tab = new TabPage("设置");
        var scroll = new Panel { Dock = DockStyle.Fill, AutoScroll = true };
        var panel = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 2, RowCount = 20, Padding = new Padding(10), Width = 540 };
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 160));
        panel.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));

        int row = 0;

        // Monitor settings
        panel.Controls.Add(CreateSectionLabel("监控"), 0, row); panel.SetColumnSpan(panel.Controls[panel.Controls.Count - 1], 2); row++;

        panel.Controls.Add(CreateFieldLabel("检测间隔(秒):"), 0, row);
        _numInterval = new NumericUpDown { Minimum = 1, Maximum = 60, Value = _config.CheckIntervalSeconds, Dock = DockStyle.Left, Width = 80 };
        panel.Controls.Add(_numInterval, 1, row); row++;

        panel.Controls.Add(CreateFieldLabel("TAP Metric:"), 0, row);
        _numTapMetric = new NumericUpDown { Minimum = 1, Maximum = 9999, Value = _config.TapMetric, Dock = DockStyle.Left, Width = 80 };
        panel.Controls.Add(_numTapMetric, 1, row); row++;

        panel.Controls.Add(CreateFieldLabel("主网卡 Metric:"), 0, row);
        _numMainMetric = new NumericUpDown { Minimum = 1, Maximum = 9999, Value = _config.MainMetric, Dock = DockStyle.Left, Width = 80 };
        panel.Controls.Add(_numMainMetric, 1, row); row++;

        // Behavior
        panel.Controls.Add(CreateSectionLabel("行为"), 0, row); panel.SetColumnSpan(panel.Controls[panel.Controls.Count - 1], 2); row++;

        _chkAutoStart = new CheckBox { Text = "开机自启", Checked = _config.AutoStart, Dock = DockStyle.Left };
        _chkAutoStart.CheckedChanged += (s, e) => SetAutoStart(_chkAutoStart.Checked);
        panel.Controls.Add(CreateFieldLabel("自动启动:"), 0, row);
        panel.Controls.Add(_chkAutoStart, 1, row); row++;

        _chkMinimizeTray = new CheckBox { Text = "最小化到托盘", Checked = _config.MinimizeToTray, Dock = DockStyle.Left };
        panel.Controls.Add(CreateFieldLabel("最小化:"), 0, row);
        panel.Controls.Add(_chkMinimizeTray, 1, row); row++;

        _chkNotify = new CheckBox { Text = "修复时显示通知", Checked = _config.ShowNotification, Dock = DockStyle.Left };
        panel.Controls.Add(CreateFieldLabel("通知:"), 0, row);
        panel.Controls.Add(_chkNotify, 1, row); row++;

        // Private networks
        panel.Controls.Add(CreateSectionLabel("内网网段"), 0, row); panel.SetColumnSpan(panel.Controls[panel.Controls.Count - 1], 2); row++;

        _lbPrivateNets = new ListBox { Dock = DockStyle.Top, Height = 70 };
        _config.PrivateNets.ForEach(n => _lbPrivateNets.Items.Add(n));
        var netBtnPanel = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 30 };
        var btnAddNet = new Button { Text = "添加", Size = new Size(60, 26) };
        var btnDelNet = new Button { Text = "删除", Size = new Size(60, 26) };
        btnAddNet.Click += (s, e) => AddPrivateNet();
        btnDelNet.Click += (s, e) => RemovePrivateNet();
        netBtnPanel.Controls.Add(btnAddNet);
        netBtnPanel.Controls.Add(btnDelNet);

        var netContainer = new Panel { Dock = DockStyle.Fill };
        netContainer.Controls.Add(_lbPrivateNets);
        netContainer.Controls.Add(netBtnPanel);
        panel.Controls.Add(CreateFieldLabel("IP 范围:"), 0, row);
        panel.Controls.Add(netContainer, 1, row); row++;

        // Custom routes
        panel.Controls.Add(CreateSectionLabel("自定义路由规则"), 0, row); panel.SetColumnSpan(panel.Controls[panel.Controls.Count - 1], 2); row++;

        _dgCustomRoutes = new DataGridView
        {
            Dock = DockStyle.Fill,
            Height = 100,
            AllowUserToAddRows = true,
            AllowUserToDeleteRows = true,
            AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
            ColumnCount = 2
        };
        _dgCustomRoutes.Columns[0].Name = "Prefix";
        _dgCustomRoutes.Columns[0].HeaderText = "IP 前缀 (如 10.0.0.0/8)";
        _dgCustomRoutes.Columns[1].Name = "Via";
        _dgCustomRoutes.Columns[1].HeaderText = "出口 (tap/main)";
        _dgCustomRoutes.Columns[1].Width = 80;

        foreach (var r in _config.CustomRoutes)
            _dgCustomRoutes.Rows.Add(r.Key, r.Value);

        panel.Controls.Add(CreateFieldLabel("规则:"), 0, row);
        panel.Controls.Add(_dgCustomRoutes, 1, row); row++;

        // Bark push
        panel.Controls.Add(CreateSectionLabel("Bark 推送"), 0, row); panel.SetColumnSpan(panel.Controls[panel.Controls.Count - 1], 2); row++;

        _chkBark = new CheckBox { Text = "启用 Bark 推送", Checked = _config.BarkEnabled, Dock = DockStyle.Left };
        panel.Controls.Add(CreateFieldLabel("Bark:"), 0, row);
        panel.Controls.Add(_chkBark, 1, row); row++;

        panel.Controls.Add(CreateFieldLabel("服务器:"), 0, row);
        _txtBarkServer = new TextBox { Text = _config.BarkServer, Dock = DockStyle.Fill, PlaceholderText = "https://api.day.app" };
        panel.Controls.Add(_txtBarkServer, 1, row); row++;

        panel.Controls.Add(CreateFieldLabel("设备密钥:"), 0, row);
        _txtBarkKey = new TextBox { Text = _config.BarkDeviceKey, Dock = DockStyle.Fill, PlaceholderText = "你的 Bark deviceKey" };
        panel.Controls.Add(_txtBarkKey, 1, row); row++;

        // Save button
        row++;
        var btnSave = new Button { Text = "保存设置", Size = new Size(120, 32), Margin = new Padding(0, 10, 0, 0) };
        btnSave.Click += SaveSettings;
        panel.Controls.Add(btnSave, 1, row);

        scroll.Controls.Add(panel);
        tab.Controls.Add(scroll);
        _tabControl.TabPages.Add(tab);
    }

    // ===== Log Tab =====

    private void BuildLogTab()
    {
        var tab = new TabPage("日志");
        var panel = new Panel { Dock = DockStyle.Fill, Padding = new Padding(10) };

        _txtLog = new TextBox
        {
            Dock = DockStyle.Fill,
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Both,
            Font = new Font("Consolas", 9),
            BackColor = Color.FromArgb(30, 30, 30),
            ForeColor = Color.LightGray
        };

        var btnPanel = new FlowLayoutPanel { Dock = DockStyle.Bottom, FlowDirection = FlowDirection.LeftToRight, Height = 40 };
        _btnExportLog = new Button { Text = "导出日志", Size = new Size(100, 30) };
        _btnClearLog = new Button { Text = "清空日志", Size = new Size(100, 30) };
        _btnExportLog.Click += (s, e) => ExportLog();
        _btnClearLog.Click += (s, e) => ClearLog();
        btnPanel.Controls.Add(_btnExportLog);
        btnPanel.Controls.Add(_btnClearLog);

        panel.Controls.Add(_txtLog);
        panel.Controls.Add(btnPanel);
        tab.Controls.Add(panel);
        _tabControl.TabPages.Add(tab);

        // Load existing logs
        _txtLog.Text = Logger.GetRecentLogs();
    }

    // ===== About Tab =====

    private void BuildAboutTab()
    {
        var tab = new TabPage("关于");
        var panel = new TableLayoutPanel { Dock = DockStyle.Fill, ColumnCount = 1, RowCount = 6, Padding = new Padding(20) };

        var title = new Label { Text = "GatewayPolicy", Font = new Font("Segoe UI", 18, FontStyle.Bold), AutoSize = true };
        var ver = new Label { Text = $"v{_config.AppVersion}", Font = new Font("Segoe UI", 10), AutoSize = true, ForeColor = Color.Gray };
        var desc = new Label
        {
            Text = "内网流量走VPN，公网流量走本地网卡。\n防止VPN路由劫持导致公网中断。",
            AutoSize = true
        };

        var btnCheckUpdate = new Button { Text = "检查更新", Size = new Size(150, 30), Margin = new Padding(0, 20, 0, 0) };
        btnCheckUpdate.Click += async (s, e) => await CheckUpdate();

        panel.Controls.Add(title);
        panel.Controls.Add(ver);
        panel.Controls.Add(desc);
        panel.Controls.Add(btnCheckUpdate);

        tab.Controls.Add(panel);
        _tabControl.TabPages.Add(tab);
    }

    // ===== Actions =====

    private void ToggleMonitor()
    {
        if (_guard.State == MonitorState.Stopped)
        {
            _guard.Start();
            _btnToggle.Text = "停止监控";
        }
        else
        {
            _guard.Stop();
            _btnToggle.Text = "启动监控";
        }
        RefreshStatus();
    }

    private void FixNow()
    {
        if (_guard.State == MonitorState.Stopped)
        {
            _guard.Start();
            _btnToggle.Text = "停止监控";
        }
        Logger.Write("手动修复触发", LogLevel.Info);
        _guard.DoCheck();
    }

    private void SaveSettings(object? sender, EventArgs e)
    {
        _config.CheckIntervalSeconds = (int)_numInterval.Value;
        _config.TapMetric = (int)_numTapMetric.Value;
        _config.MainMetric = (int)_numMainMetric.Value;
        _config.AutoStart = _chkAutoStart.Checked;
        _config.MinimizeToTray = _chkMinimizeTray.Checked;
        _config.ShowNotification = _chkNotify.Checked;
        _config.BarkEnabled = _chkBark.Checked;
        _config.BarkServer = _txtBarkServer.Text.Trim();
        _config.BarkDeviceKey = _txtBarkKey.Text.Trim();

        // Private nets
        _config.PrivateNets.Clear();
        foreach (var item in _lbPrivateNets.Items)
            if (item.ToString() is string s && !string.IsNullOrWhiteSpace(s))
                _config.PrivateNets.Add(s);

        // Custom routes
        _config.CustomRoutes.Clear();
        foreach (DataGridViewRow row in _dgCustomRoutes.Rows)
        {
            if (row.IsNewRow) continue;
            var prefix = row.Cells[0].Value?.ToString()?.Trim();
            var via = row.Cells[1].Value?.ToString()?.Trim()?.ToLower();
            if (!string.IsNullOrEmpty(prefix) && (via == "tap" || via == "main"))
                _config.CustomRoutes[prefix] = via;
        }

        _config.Save();
        SetAutoStart(_config.AutoStart);
        Logger.Write("设置已保存", LogLevel.Info);
        MessageBox.Show("设置已保存。", "GatewayPolicy", MessageBoxButtons.OK, MessageBoxIcon.Information);
    }

    private void AddPrivateNet()
    {
        var input = ShowInputDialog("添加内网网段", "输入 IP 前缀 (如 10.0.0.0/8):");
        if (!string.IsNullOrWhiteSpace(input))
        {
            _lbPrivateNets.Items.Add(input.Trim());
        }
    }

    private static string ShowInputDialog(string title, string prompt)
    {
        using var form = new Form { Text = title, Size = new Size(400, 150), StartPosition = FormStartPosition.CenterParent, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false };
        var lbl = new Label { Text = prompt, Dock = DockStyle.Top, Padding = new Padding(10) };
        var txt = new TextBox { Dock = DockStyle.Top, Padding = new Padding(10) };
        var btn = new Button { Text = "确定", DialogResult = DialogResult.OK, Dock = DockStyle.Bottom };
        form.Controls.Add(btn);
        form.Controls.Add(txt);
        form.Controls.Add(lbl);
        form.AcceptButton = btn;
        return form.ShowDialog() == DialogResult.OK ? txt.Text : "";
    }

    private void RemovePrivateNet()
    {
        if (_lbPrivateNets.SelectedIndex >= 0)
            _lbPrivateNets.Items.RemoveAt(_lbPrivateNets.SelectedIndex);
    }

    private void SetAutoStart(bool enable)
    {
        try
        {
            using var key = Registry.CurrentUser.OpenSubKey(@"SOFTWARE\Microsoft\Windows\CurrentVersion\Run", true);
            if (key == null) return;
            var exePath = Application.ExecutablePath;
            if (enable)
                key.SetValue("GatewayPolicy", $"\"{exePath}\"");
            else
                key.DeleteValue("GatewayPolicy", false);
        }
        catch (Exception ex)
        {
            Logger.Write($"AutoStart registry error: {ex.Message}", LogLevel.Warning);
        }
    }

    private async Task CheckUpdate(bool silent = false)
    {
        var repo = _config.UpdateRepo;
        if (string.IsNullOrEmpty(repo))
        {
            if (!silent) MessageBox.Show("更新仓库未配置。\n请在 config.json 中设置 UpdateRepo (格式: owner/repo)。", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(15) };
            http.DefaultRequestHeaders.Add("User-Agent", "GatewayPolicy");
            var url = $"https://api.github.com/repos/{repo}/releases/latest";
            var json = await http.GetStringAsync(url);
            var release = System.Text.Json.JsonDocument.Parse(json);
            var tagName = release.RootElement.GetProperty("tag_name").GetString() ?? "";

            if (string.IsNullOrEmpty(tagName) || tagName == $"v{_config.AppVersion}")
            {
                if (!silent) MessageBox.Show("已是最新版本。", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }

            // Find exe asset
            string? downloadUrl = null;
            string? assetName = null;
            if (release.RootElement.TryGetProperty("assets", out var assets))
            {
                foreach (var asset in assets.EnumerateArray())
                {
                    var name = asset.GetProperty("name").GetString() ?? "";
                    if (name.EndsWith(".exe", StringComparison.OrdinalIgnoreCase))
                    {
                        downloadUrl = asset.GetProperty("browser_download_url").GetString();
                        assetName = name;
                        break;
                    }
                }
            }

            if (downloadUrl == null)
            {
                if (!silent) MessageBox.Show($"发现新版本 {tagName}，但未找到可下载的文件。\n请前往 GitHub 手动下载。", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            var result = MessageBox.Show(
                $"发现新版本: {tagName} (当前: v{_config.AppVersion})\n文件: {assetName}\n\n是否立即下载并更新？",
                "发现新版本", MessageBoxButtons.YesNo, MessageBoxIcon.Information);

            if (result == DialogResult.Yes)
            {
                await DownloadAndInstallUpdate(downloadUrl, assetName);
            }
        }
        catch (Exception ex)
        {
            if (!silent) MessageBox.Show($"检查更新失败: {ex.Message}", "检查更新", MessageBoxButtons.OK, MessageBoxIcon.Warning);
        }
    }

    private async Task DownloadAndInstallUpdate(string downloadUrl, string fileName)
    {
        try
        {
            var tempDir = Path.Combine(Path.GetTempPath(), "GatewayPolicy_Update");
            Directory.CreateDirectory(tempDir);
            var tempFile = Path.Combine(tempDir, fileName);

            Logger.Write($"正在下载更新: {fileName}", LogLevel.Info);

            using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
            var data = await http.GetByteArrayAsync(downloadUrl);
            await File.WriteAllBytesAsync(tempFile, data);

            Logger.Write($"下载完成: {tempFile}", LogLevel.Info);

            // Create updater script
            var currentExe = Application.ExecutablePath;
            var updaterBat = Path.Combine(tempDir, "updater.bat");
            var script = $@"@echo off
echo GatewayPolicy Updater
timeout /t 2 /nobreak >nul
:retry
del /f /q ""{currentExe}"" 2>nul
if exist ""{currentExe}"" (
    timeout /t 1 /nobreak >nul
    goto retry
)
copy /y ""{tempFile}"" ""{currentExe}"" >nul
start """" ""{currentExe}""
del /f /q ""%~f0""
";
            File.WriteAllText(updaterBat, script);

            MessageBox.Show("下载完成，点击确定后程序将自动重启更新。", "更新就绪", MessageBoxButtons.OK, MessageBoxIcon.Information);

            // Run updater and exit
            Process.Start(new ProcessStartInfo
            {
                FileName = updaterBat,
                CreateNoWindow = true,
                UseShellExecute = false
            });

            Application.Exit();
        }
        catch (Exception ex)
        {
            MessageBox.Show($"更新失败: {ex.Message}", "更新错误", MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void ExportLog()
    {
        var sfd = new SaveFileDialog { Filter = "日志文件|*.log|文本文件|*.txt|所有文件|*.*", FileName = $"GatewayPolicy_{DateTime.Now:yyyyMMdd}.log" };
        if (sfd.ShowDialog() == DialogResult.OK)
        {
            File.WriteAllText(sfd.FileName, Logger.ExportAllLogs());
            MessageBox.Show("日志已导出。", "GatewayPolicy", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
    }

    private void ClearLog()
    {
        if (MessageBox.Show("确认清空所有日志？", "GatewayPolicy", MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes)
        {
            Logger.ClearLogs();
            _txtLog.Clear();
        }
    }

    // ===== Events =====

    private void OnGuardLog(string message, LogLevel level)
    {
        if (_txtLog.IsDisposed) return;
        try
        {
            _txtLog.Invoke(() =>
            {
                if (_txtLog.Lines.Length > 2000)
                    _txtLog.Text = _txtLog.Text.Substring(_txtLog.Text.IndexOf('\n') + 1);
                _txtLog.AppendText(message + Environment.NewLine);
            });
        }
        catch { }
    }

    private void OnGuardStateChanged(MonitorState state)
    {
        try
        {
            this.Invoke(() =>
            {
                RefreshStatus();
                _notifyIcon.Icon = state switch
                {
                    MonitorState.Running => _iconGreen,
                    MonitorState.Fixing => _iconYellow,
                    _ => _iconRed
                };
                _trayMenu.Items[0].Text = $"Status: {state}";
            });
        }
        catch { }
    }

    private void OnGuardFixCompleted()
    {
        if (_config.ShowNotification)
        {
            try
            {
                this.Invoke(() => _notifyIcon.ShowBalloonTip(3000, "GatewayPolicy", "路由修复完成，公网已恢复。", ToolTipIcon.Info));
            }
            catch { }
        }
    }

    private void RefreshStatus()
    {
        try
        {
            var nics = System.Net.NetworkInformation.NetworkInterface.GetAllNetworkInterfaces();
            var mainNic = nics.FirstOrDefault(n =>
                n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up &&
                !n.Description.Contains("TAP") && !n.Description.Contains("VPN") &&
                !n.Description.Contains("Tunnel") && !n.Description.Contains("Virtual") &&
                n.NetworkInterfaceType != System.Net.NetworkInformation.NetworkInterfaceType.Loopback);
            var tapNic = nics.FirstOrDefault(n =>
                n.OperationalStatus == System.Net.NetworkInformation.OperationalStatus.Up &&
                (n.Description.Contains("TAP") || n.Description.Contains("VPN") || n.Description.Contains("Tunnel")));

            _lblState.Text = _guard.State.ToString();
            _lblState.ForeColor = _guard.State switch
            {
                MonitorState.Running => Color.Green,
                MonitorState.Fixing => Color.Gold,
                _ => Color.Red
            };
            var stateText = _guard.State switch
            {
                MonitorState.Running => "运行中",
                MonitorState.Fixing => "修复中",
                _ => "已停止"
            };
            _lblState.Text = stateText;
            _lblMainNic.Text = mainNic != null ? $"{mainNic.Name} ({mainNic.Description})" : "未找到";
            _lblTapNic.Text = tapNic != null ? $"{tapNic.Name} ({tapNic.Description})" : "未找到";
            _lblLastFix.Text = _config.LastFixTime?.ToString("yyyy-MM-dd HH:mm:ss") ?? "-";
            _lblTotalFixes.Text = _config.TotalFixes.ToString();

            _btnToggle.Text = _guard.State == MonitorState.Stopped ? "启动监控" : "停止监控";
        }
        catch { }
    }

    // ===== Form lifecycle =====

    protected override void OnFormClosing(FormClosingEventArgs e)
    {
        if (_config.MinimizeToTray && e.CloseReason == CloseReason.UserClosing)
        {
            e.Cancel = true;
            Hide();
            return;
        }
        base.OnFormClosing(e);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _guard.Stop();
            _notifyIcon.Visible = false;
            _notifyIcon.Dispose();
            _iconGreen?.Dispose();
            _iconYellow?.Dispose();
            _iconRed?.Dispose();
        }
        base.Dispose(disposing);
    }

    private void ShowWindow()
    {
        Show();
        WindowState = FormWindowState.Normal;
        BringToFront();
    }

    private void ExitApp()
    {
        _config.MinimizeToTray = false; // Allow close
        Application.Exit();
    }

    // ===== Helper controls =====

    private static Label CreateFieldLabel(string text) => new() { Text = text, AutoSize = true, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft };
    private static Label CreateValueLabel(string text) => new() { Text = text, AutoSize = true, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleLeft, Font = new Font("Segoe UI", 9) };
    private static Label CreateSectionLabel(string text) => new() { Text = text, Font = new Font("Segoe UI", 10, FontStyle.Bold), AutoSize = true, ForeColor = Color.FromArgb(0, 100, 200), Margin = new Padding(0, 10, 0, 4) };
}
