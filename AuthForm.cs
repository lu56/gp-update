using System.Drawing;
using System.Windows.Forms;
using System.Text.Json;

namespace GatewayPolicy;

public class AuthForm : Form
{
    private readonly string _deviceId;
    private readonly string _serverUrl;
    private readonly AppConfig _config;
    private Label _lblStatus = null!;
    private Button _btnAuth = null!;
    private Button _btnCopy = null!;
    private TextBox _txtDeviceId = null!;
    private bool _authorized = false;

    public bool Authorized => _authorized;

    public AuthForm(string deviceId, string serverUrl, AppConfig config)
    {
        _deviceId = deviceId;
        _serverUrl = serverUrl;
        _config = config;
        InitializeForm();
    }

    private void InitializeForm()
    {
        Text = "GatewayPolicy - 设备授权";
        Size = new Size(480, 340);
        StartPosition = FormStartPosition.CenterScreen;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;

        var panel = new TableLayoutPanel
        {
            Dock = DockStyle.Fill,
            ColumnCount = 1,
            RowCount = 6,
            Padding = new Padding(20)
        };

        // Title
        var title = new Label
        {
            Text = "设备授权验证",
            Font = new Font("Microsoft YaHei UI", 14, FontStyle.Bold),
            AutoSize = true,
            Dock = DockStyle.Left,
            Margin = new Padding(0, 0, 0, 10)
        };
        panel.Controls.Add(title);

        // Instructions
        var desc = new Label
        {
            Text = "请将下方机器码发送给管理员，待管理员录入后点击\"验证授权\"。",
            AutoSize = true,
            Dock = DockStyle.Left,
            Margin = new Padding(0, 0, 0, 10)
        };
        panel.Controls.Add(desc);

        // Device ID display
        var idPanel = new Panel
        {
            Dock = DockStyle.Top,
            Height = 36,
            Margin = new Padding(0, 0, 0, 6)
        };
        _txtDeviceId = new TextBox
        {
            Text = _deviceId,
            ReadOnly = true,
            Dock = DockStyle.Fill,
            Font = new Font("Consolas", 10),
            BackColor = Color.FromArgb(245, 245, 245)
        };
        _btnCopy = new Button
        {
            Text = "复制",
            Dock = DockStyle.Right,
            Width = 60
        };
        _btnCopy.Click += (s, e) =>
        {
            Clipboard.SetText(_deviceId);
            _btnCopy.Text = "已复制";
            var timer = new System.Windows.Forms.Timer { Interval = 1500 };
            timer.Tick += (_, __) => { _btnCopy.Text = "复制"; timer.Dispose(); };
            timer.Start();
        };
        idPanel.Controls.Add(_txtDeviceId);
        idPanel.Controls.Add(_btnCopy);
        panel.Controls.Add(idPanel);

        // Server URL display
        var lblServer = new Label
        {
            Text = $"授权服务器: {_serverUrl}",
            AutoSize = true,
            Dock = DockStyle.Left,
            ForeColor = Color.Gray,
            Font = new Font("Microsoft YaHei UI", 8),
            Margin = new Padding(0, 0, 0, 10)
        };
        panel.Controls.Add(lblServer);

        // Status
        _lblStatus = new Label
        {
            Text = "",
            AutoSize = true,
            Dock = DockStyle.Left,
            ForeColor = Color.Blue,
            Margin = new Padding(0, 0, 0, 10)
        };
        panel.Controls.Add(_lblStatus);

        // Buttons
        var btnPanel = new FlowLayoutPanel
        {
            Dock = DockStyle.Left,
            FlowDirection = FlowDirection.LeftToRight
        };
        _btnAuth = new Button
        {
            Text = "验证授权",
            Size = new Size(120, 36),
            BackColor = Color.FromArgb(74, 108, 247),
            ForeColor = Color.White,
            FlatStyle = FlatStyle.Flat
        };
        _btnAuth.FlatAppearance.BorderSize = 0;
        _btnAuth.Click += DoAuth;

        btnPanel.Controls.Add(_btnAuth);
        panel.Controls.Add(btnPanel);

        Controls.Add(panel);

        // Check cached token first
        if (HasValidCache())
        {
            _authorized = true;
        }
    }

    private bool HasValidCache()
    {
        try
        {
            var cache = _config.AuthToken;
            var expire = _config.AuthExpire;
            if (string.IsNullOrEmpty(cache) || string.IsNullOrEmpty(expire)) return false;
            if (DateTime.TryParse(expire, out var exp) && DateTime.Now < exp)
                return true;
        }
        catch { }
        return false;
    }

    private async void DoAuth(object? sender, EventArgs e)
    {
        _btnAuth.Enabled = false;
        _lblStatus.Text = "正在验证...";
        _lblStatus.ForeColor = Color.Blue;

        try
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(10) };
            var body = JsonSerializer.Serialize(new { device_id = _deviceId });
            var content = new StringContent(body, System.Text.Encoding.UTF8, "application/json");
            var resp = await http.PostAsync($"{_serverUrl}/api/auth/check", content);
            var json = await resp.Content.ReadAsStringAsync();

            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            var ok = root.GetProperty("ok").GetBoolean();
            var msg = root.GetProperty("message").GetString() ?? "";

            if (ok)
            {
                var token = root.GetProperty("data").GetProperty("token").GetString() ?? "";
                _config.AuthToken = token;
                _config.AuthExpire = DateTime.Now.AddDays(7).ToString("yyyy-MM-dd HH:mm:ss");
                _config.Save();

                _lblStatus.Text = "授权验证通过！";
                _lblStatus.ForeColor = Color.Green;
                _authorized = true;

                await Task.Delay(800);
                DialogResult = DialogResult.OK;
                Close();
            }
            else
            {
                _lblStatus.Text = $"验证失败: {msg}";
                _lblStatus.ForeColor = Color.Red;
                _btnAuth.Enabled = true;
            }
        }
        catch (Exception ex)
        {
            _lblStatus.Text = $"连接失败: {ex.Message}";
            _lblStatus.ForeColor = Color.Red;
            _btnAuth.Enabled = true;
        }
    }
}
