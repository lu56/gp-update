using System.Windows.Forms;

namespace GatewayPolicy;

static class Program
{
    [STAThread]
    static void Main()
    {
        Application.EnableVisualStyles();
        Application.SetCompatibleTextRenderingDefault(false);

        var config = AppConfig.Load();

        // Device fingerprint
        var deviceId = DeviceFingerprint.Generate();

        // Auth check
        if (!string.IsNullOrEmpty(config.AuthServer))
        {
            // Check cached token
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
                using var authForm = new AuthForm(deviceId, config.AuthServer, config);
                if (authForm.ShowDialog() != DialogResult.OK)
                {
                    // Auth failed or cancelled
                    return;
                }
            }
        }

        var guard = new RouteGuard(config);

        // Wire logger
        guard.OnLog += (message, level) => Logger.Write(message, level);

        using var form = new MainForm(config, guard);

        // Auto-start monitoring
        if (config.AutoStart)
        {
            guard.Start();
        }

        Application.Run(form);
    }
}
