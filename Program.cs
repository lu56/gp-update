using System.Windows.Forms;

namespace GatewayPolicy;

static class Program
{
    // Re-validate token every 30 minutes
    private const int RevalidateIntervalMinutes = 30;
    private static System.Threading.Timer? _revalidateTimer;
    private static RouteGuard? _guard;
    private static AppConfig? _config;
    private static string? _deviceId;

    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        var config = AppConfig.Load();
        _config = config;

        // Device fingerprint
        var deviceId = DeviceFingerprint.Generate();
        _deviceId = deviceId;

        // ===== MANDATORY AUTH =====
        // Auth server is hardcoded — cannot be bypassed by editing config.json
        var authServer = config.EffectiveAuthServer;

        bool hasValidCache = false;
        try
        {
            if (!string.IsNullOrEmpty(config.AuthToken) && !string.IsNullOrEmpty(config.AuthExpire))
            {
                if (DateTime.TryParse(config.AuthExpire, out var exp) && DateTime.Now < exp)
                    hasValidCache = true;
            }
        }
        catch { }

        if (!hasValidCache)
        {
            using var authForm = new AuthForm(deviceId, authServer, config);
            if (authForm.ShowDialog() != DialogResult.OK)
                return;
        }
        else
        {
            // Have cached token — verify it's still valid with the server (silent)
            if (!VerifyTokenSilent(deviceId, authServer, config))
            {
                using var authForm = new AuthForm(deviceId, authServer, config);
                if (authForm.ShowDialog() != DialogResult.OK)
                    return;
            }
        }

        var guard = new RouteGuard(config);
        _guard = guard;

        // Wire logger
        guard.OnLog += (message, level) => Logger.Write(message, level);

        // Start periodic re-validation
        StartRevalidationTimer(deviceId, authServer, config, guard);

        using var form = new MainForm(config, guard);

        Application.Run(form);
    }

    private static void DebugWrite(string msg)
    {
        try { File.AppendAllText(Path.Combine(Path.GetTempPath(), "gp_debug.log"), $"[{DateTime.Now:HH:mm:ss.fff}] {msg}{Environment.NewLine}"); } catch { }
    }

    private static void StartRevalidationTimer(string deviceId, string authServer, AppConfig config, RouteGuard guard)
    {
        _revalidateTimer = new System.Threading.Timer(_ =>
        {
            try
            {
                if (!VerifyTokenSilent(deviceId, authServer, config))
                {
                    // Token revoked — stop monitoring and force re-auth
                    Logger.Write("授权已被撤销，停止监控", LogLevel.Warning);
                    guard.Stop();

                    // Show auth dialog on UI thread
                    if (System.Windows.Forms.Application.OpenForms.Count > 0)
                    {
                        var mainForm = System.Windows.Forms.Application.OpenForms[0];
                        mainForm.Invoke(() =>
                        {
                            MessageBox.Show("设备授权已被撤销，请重新验证。", "GatewayPolicy", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                            using var authForm = new AuthForm(deviceId, authServer, config);
                            if (authForm.ShowDialog() == DialogResult.OK)
                            {
                                guard.Start();
                                Logger.Write("重新授权成功，恢复监控", LogLevel.Info);
                            }
                            else
                            {
                                System.Windows.Forms.Application.Exit();
                            }
                        });
                    }
                }
                else
                {
                    // Token still valid — refresh expiry
                    config.AuthExpire = DateTime.Now.AddDays(2).ToString("yyyy-MM-dd HH:mm:ss");
                    config.Save();
                }
            }
            catch (Exception ex)
            {
                Logger.Write($"Token re-validation error: {ex.Message}", LogLevel.Warning);
            }
        }, null, TimeSpan.FromMinutes(RevalidateIntervalMinutes), TimeSpan.FromMinutes(RevalidateIntervalMinutes));
    }

    /// <summary>
    /// Silently verify the cached token with the server.
    /// Returns true if the device is still authorized.
    /// </summary>
    private static bool VerifyTokenSilent(string deviceId, string authServer, AppConfig config)
    {
        try
        {
            using var http = new HttpClient(new HttpClientHandler
            {
                ServerCertificateCustomValidationCallback = (sender, cert, chain, sslPolicyErrors) => true
            });
            http.Timeout = TimeSpan.FromSeconds(8);

            var body = System.Text.Json.JsonSerializer.Serialize(new { device_id = deviceId });
            var content = new StringContent(body, System.Text.Encoding.UTF8, "application/json");
            var resp = http.PostAsync($"{authServer}/api/auth/check", content).Result;
            var json = resp.Content.ReadAsStringAsync().Result;

            using var doc = System.Text.Json.JsonDocument.Parse(json);
            var ok = doc.RootElement.GetProperty("ok").GetBoolean();

            if (ok)
            {
                // Refresh token
                var token = doc.RootElement.GetProperty("data").GetProperty("token").GetString() ?? "";
                config.AuthToken = token;
                config.AuthExpire = DateTime.Now.AddDays(2).ToString("yyyy-MM-dd HH:mm:ss");
                config.Save();
                return true;
            }

            // Not ok — clear cache
            config.AuthToken = "";
            config.AuthExpire = "";
            config.Save();
            return false;
        }
        catch
        {
            // Network error — trust the cached token for now (grace period)
            // If cache is still within expiry, allow continued operation
            if (!string.IsNullOrEmpty(config.AuthExpire) &&
                DateTime.TryParse(config.AuthExpire, out var exp) && DateTime.Now < exp)
            {
                return true;
            }
            return false;
        }
    }
}
