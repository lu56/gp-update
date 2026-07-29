using System.Text.Json;
using System.Text.Json.Serialization;

namespace GatewayPolicy;

public class AppConfig
{
    // App version (must match csproj Version)
    public string AppVersion { get; set; } = "1.1.0";

    // Monitor
    public int CheckIntervalSeconds { get; set; } = 4;
    public int TapMetric { get; set; } = 50;
    public int MainMetric { get; set; } = 16;

    // Private networks
    public List<string> PrivateNets { get; set; } = new() { "10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10" };

    // Custom route rules: prefix -> "tap" or "main"
    public Dictionary<string, string> CustomRoutes { get; set; } = new();

    // Behavior
    public bool AutoStart { get; set; } = true;
    public bool MinimizeToTray { get; set; } = true;
    public bool ShowNotification { get; set; } = true;

    // Bark push
    public bool BarkEnabled { get; set; } = false;
    public string BarkServer { get; set; } = "";
    public string BarkDeviceKey { get; set; } = "";
    public string BarkTitle { get; set; } = "GatewayPolicy";
    public string BarkSound { get; set; } = "";

    // Auto update
    public bool AutoUpdateCheck { get; set; } = true;
    public string UpdateRepo { get; set; } = "";

    // Auth
    public string AuthServer { get; set; } = "";
    public string AuthToken { get; set; } = "";
    public string AuthExpire { get; set; } = "";

    // State
    public long TotalFixes { get; set; } = 0;
    public DateTime? LastFixTime { get; set; }

    private static readonly string ConfigPath = Path.Combine(
        AppDomain.CurrentDomain.BaseDirectory, "config.json");

    public static AppConfig Load()
    {
        if (File.Exists(ConfigPath))
        {
            try
            {
                var json = File.ReadAllText(ConfigPath);
                return JsonSerializer.Deserialize<AppConfig>(json, JsonOpts) ?? new AppConfig();
            }
            catch { }
        }
        return new AppConfig();
    }

    public void Save()
    {
        var json = JsonSerializer.Serialize(this, JsonOpts);
        File.WriteAllText(ConfigPath, json);
    }

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        WriteIndented = true,
        Converters = { new JsonStringEnumConverter() }
    };
}
