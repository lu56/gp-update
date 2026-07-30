using System.Text.Json;
using System.Text.Json.Serialization;

namespace GatewayPolicy;

public class AppConfig
{
    // Hardcoded auth server — cannot be bypassed by editing config.json
    public const string DefaultAuthServer = "https://pve.lu56.top:12233";

    // App version — hardcoded, never loaded from config.json
    private const string CompiledVersion = "5.0.3";
    [JsonIgnore]
    public string AppVersion => CompiledVersion;

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

    // Auth (server URL is enforced via DefaultAuthServer constant; config value only overrides if non-empty)
    public string AuthServer { get; set; } = "";
    public string AuthToken { get; set; } = "";
    public string AuthExpire { get; set; } = "";

    // Returns the effective auth server: config override if set, otherwise hardcoded default
    [JsonIgnore]
    public string EffectiveAuthServer => !string.IsNullOrEmpty(AuthServer) ? AuthServer : DefaultAuthServer;

    // State
    public long TotalFixes { get; set; } = 0;

    [JsonConverter(typeof(SafeNullableDateTimeConverter))]
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
                var opts = new JsonSerializerOptions(JsonOpts);
                opts.NumberHandling = JsonNumberHandling.AllowReadingFromString;
                return JsonSerializer.Deserialize<AppConfig>(json, opts) ?? new AppConfig();
            }
            catch
            {
                // Config file might be from a different version or corrupted.
                // Try partial load — at least preserve auth info.
                try
                {
                    var json = File.ReadAllText(ConfigPath);
                    using var doc = JsonDocument.Parse(json);
                    var fallback = new AppConfig();
                    if (doc.RootElement.TryGetProperty("AuthToken", out var t) && t.ValueKind == JsonValueKind.String)
                        fallback.AuthToken = t.GetString() ?? "";
                    if (doc.RootElement.TryGetProperty("AuthExpire", out var e) && e.ValueKind == JsonValueKind.String)
                        fallback.AuthExpire = e.GetString() ?? "";
                    if (doc.RootElement.TryGetProperty("AuthServer", out var s) && s.ValueKind == JsonValueKind.String)
                        fallback.AuthServer = s.GetString() ?? "";
                    return fallback;
                }
                catch { }
            }
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

/// <summary>
/// JSON converter for DateTime? that gracefully handles empty strings (treats them as null).
/// </summary>
public class SafeNullableDateTimeConverter : JsonConverter<DateTime?>
{
    public override DateTime? Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options)
    {
        if (reader.TokenType == JsonTokenType.Null) return null;
        if (reader.TokenType == JsonTokenType.String)
        {
            var s = reader.GetString();
            if (string.IsNullOrWhiteSpace(s)) return null;
            if (DateTime.TryParse(s, out var dt)) return dt;
            return null;
        }
        if (reader.TokenType == JsonTokenType.False) return null;
        return reader.GetDateTime();
    }

    public override void Write(Utf8JsonWriter writer, DateTime? value, JsonSerializerOptions options)
    {
        if (value.HasValue)
            writer.WriteStringValue(value.Value.ToString("yyyy-MM-dd HH:mm:ss"));
        else
            writer.WriteNullValue();
    }
}
