using System.Net.NetworkInformation;
using System.Diagnostics;
using System.Net;

namespace GatewayPolicy;

public enum MonitorState { Stopped, Running, Fixing }

public class RouteGuard
{
    private readonly AppConfig _config;
    private System.Threading.Timer? _timer;
    private int _mainIfIndex;
    private string _mainGateway = "";
    private volatile bool _fixing;
    private DateTime _lastFixTime = DateTime.MinValue;
    private string _routeTableCache = "";
    private DateTime _routeTableCacheTime = DateTime.MinValue;
    private string _lastNicLog = "";  // dedup: only log when NIC info changes
    private bool _wasHijacked = false;  // only log hijack on state transition
    private bool _steadyState = false;  // suppress logs when nothing changes

    // /2 counter-routes that override VPN's /1 hijack
    private static readonly string[] CounterRoutes = {
        "0.0.0.0/2", "64.0.0.0/2", "128.0.0.0/2", "192.0.0.0/2"
    };

    public MonitorState State { get; private set; } = MonitorState.Stopped;

    /// <summary>
    /// Current hijack status (true = VPN is hijacking routes).
    /// </summary>
    public bool IsHijacked => _wasHijacked;

    public event Action<string, LogLevel>? OnLog;
    public event Action<MonitorState>? OnStateChanged;
    public event Action? OnFixCompleted;
    public event Action<bool>? OnHijackChanged;

    public RouteGuard(AppConfig config)
    {
        _config = config;
    }

    public void Start()
    {
        if (State != MonitorState.Stopped) return;  // Already running or fixing

        CacheMainNic();

        State = MonitorState.Running;
        OnStateChanged?.Invoke(State);
        _timer = new System.Threading.Timer(_ => CheckLoop(), null,
            TimeSpan.FromSeconds(2), TimeSpan.FromSeconds(_config.CheckIntervalSeconds));
        Log("Monitor started (v2.1: /2 counter-route strategy)", LogLevel.Info);
    }

    public void Stop()
    {
        _timer?.Dispose();
        _timer = null;
        _fixing = false;
        State = MonitorState.Stopped;
        OnStateChanged?.Invoke(State);
        Log("Monitor stopped", LogLevel.Info);
    }

    /// <summary>
    /// Remove all /2 counter-routes and restore original networking.
    /// Optionally stops monitoring first. Does NOT restart monitoring.
    /// </summary>
    public void RestoreNetwork()
    {
        // Stop monitoring to avoid re-adding routes during restoration
        if (State != MonitorState.Stopped)
        {
            _timer?.Dispose();
            _timer = null;
            _fixing = false;
            State = MonitorState.Stopped;
            OnStateChanged?.Invoke(State);
        }

        int removed = 0;

        // Remove /2 counter-routes
        foreach (var prefix in CounterRoutes)
        {
            RunRoute($"delete {prefix}");
            removed++;
        }

        // Also remove private routes to TAP
        foreach (var net in _config.PrivateNets)
        {
            RunRoute($"delete {net}");
            removed++;
        }

        // Remove custom routes
        foreach (var rule in _config.CustomRoutes)
        {
            RunRoute($"delete {rule.Key}");
            removed++;
        }

        _wasHijacked = false;
        _steadyState = false;

        Log($"Network restored — removed {removed} routes (counter-routes + private routes)", LogLevel.Info);
        OnHijackChanged?.Invoke(false);
        OnFixCompleted?.Invoke();
    }

    private bool CacheMainNic()
    {
        try
        {
            var nics = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up &&
                            n.NetworkInterfaceType != NetworkInterfaceType.Loopback &&
                            !IsVirtualAdapter(n))
                .ToList();

            foreach (var nic in nics)
            {
                var ipProps = nic.GetIPProperties();
                var gateway = ipProps.GatewayAddresses.FirstOrDefault(g => g.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork);
                if (gateway != null && !string.IsNullOrEmpty(gateway.Address.ToString()))
                {
                    _mainIfIndex = nic.GetIPProperties().GetIPv4Properties().Index;
                    _mainGateway = gateway.Address.ToString();
                    // Only log when NIC info changes
                    var nicInfo = $"{nic.Name}|{_mainIfIndex}|{_mainGateway}";
                    if (_lastNicLog != nicInfo)
                    {
                        _lastNicLog = nicInfo;
                        Log($"Main NIC: {nic.Name} (if={_mainIfIndex}, gw={_mainGateway})", LogLevel.Info);
                    }
                    return true;
                }
            }
        }
        catch (Exception ex)
        {
            Log($"Detect main NIC error: {ex.Message}", LogLevel.Error);
        }
        return false;
    }

    private void CheckLoop()
    {
        if (_fixing) return;
        DoCheck();
    }

    public void DoCheck()
    {
        if (_fixing) return;

        try
        {
            CacheMainNic();

            // Primary check: def1 hijack routes (/1 routes override default route)
            bool hijacked = false;

            if (RouteExists("0.0.0.0/1") || RouteExists("128.0.0.0/1"))
            {
                hijacked = true;
                if (!_wasHijacked)
                {
                    Log("Detected VPN def1 hijack route (0.0.0.0/1 or 128.0.0.0/1)", LogLevel.Info);
                    _wasHijacked = true;
                    OnHijackChanged?.Invoke(true);
                }
            }
            else
            {
                if (_wasHijacked)
                {
                    Log("VPN hijack cleared — no longer detected", LogLevel.Info);
                    _wasHijacked = false;
                    _steadyState = false;
                    OnHijackChanged?.Invoke(false);
                }
            }

            // Secondary check: TAP has default route with very low metric
            foreach (var tap in GetManagedAdapters())
            {
                var metric = GetAdapterMetric(tap.IfIndex);
                if (metric > 0 && metric < 10)
                {
                    hijacked = true;
                    if (!_wasHijacked)
                    {
                        Log($"TAP adapter {tap.Name} has low metric default route (metric={metric})", LogLevel.Info);
                        _wasHijacked = true;
                    }
                }
            }

            if (hijacked) ApplyCounterRoutes();
        }
        catch (Exception ex)
        {
            Log($"Check error: {ex.Message}", LogLevel.Error);
        }
    }

    /// <summary>
    /// New strategy: Don't fight VPN. Let it push /1 routes.
    /// Instead, add /2 counter-routes pointing to main NIC.
    /// /2 is more specific than /1, so it wins without conflicting.
    /// VPN won't detect or fight back because it doesn't push /2 routes.
    /// </summary>
    private void ApplyCounterRoutes()
    {
        // Phase 1: Check if anything actually needs fixing (no state change)
        bool needFix = false;
        var routesToAdd = new List<(string cidr, string gateway, int ifIndex, int metric, string label)>();

        try
        {
            // Step 1: Check /2 counter-routes for public traffic → main NIC
            if (!string.IsNullOrEmpty(_mainGateway) && _mainIfIndex > 0)
            {
                foreach (var prefix in CounterRoutes)
                {
                    if (!CounterRouteExists(prefix, _mainIfIndex))
                    {
                        routesToAdd.Add((prefix, _mainGateway, _mainIfIndex, _config.MainMetric, "Counter-route"));
                        needFix = true;
                    }
                }
            }

            // Step 2: Check private networks → TAP
            var managedTaps = GetManagedAdapters();
            if (managedTaps.Count > 0)
            {
                int tapIf = managedTaps[0].IfIndex;
                string? tapGw = GetTapGateway(tapIf);

                if (!string.IsNullOrEmpty(tapGw))
                {
                    foreach (var net in _config.PrivateNets)
                    {
                        if (!PrivateRouteExists(net, tapIf))
                        {
                            routesToAdd.Add((net, tapGw, tapIf, 20, "Private route"));
                            needFix = true;
                        }
                    }
                }
            }

            // Step 3: Check custom route rules
            if (!string.IsNullOrEmpty(_mainGateway) && managedTaps.Count > 0)
            {
                int tapIf = managedTaps[0].IfIndex;
                string? tapGw = GetTapGateway(tapIf);

                foreach (var rule in _config.CustomRoutes)
                {
                    if (rule.Value == "tap" && !string.IsNullOrEmpty(tapGw))
                    {
                        if (!PrivateRouteExists(rule.Key, tapIf))
                        {
                            routesToAdd.Add((rule.Key, tapGw, tapIf, 20, "Custom route"));
                            needFix = true;
                        }
                    }
                    else if (rule.Value == "main")
                    {
                        if (!CounterRouteExists(rule.Key, _mainIfIndex))
                        {
                            routesToAdd.Add((rule.Key, _mainGateway, _mainIfIndex, _config.MainMetric, "Custom route"));
                            needFix = true;
                        }
                    }
                }
            }

            // Phase 2: Only enter Fixing state if we actually need to add routes
            if (!needFix)
            {
                if (!_steadyState)
                {
                    Log("All routes verified OK — monitoring (steady state)", LogLevel.Info);
                    _steadyState = true;
                }
                return;  // Stay in Running state, no flicker
            }

            _fixing = true;
            State = MonitorState.Fixing;
            OnStateChanged?.Invoke(State);
            _steadyState = false;

            // Phase 3: Actually add the missing routes
            foreach (var r in routesToAdd)
            {
                RunRoute($"add {r.cidr} {r.gateway} if {r.ifIndex} metric {r.metric}");
                Log($"{r.label} ADD: {r.cidr} -> if={r.ifIndex} gw={r.gateway} metric={r.metric}", LogLevel.Info);
            }

            _config.TotalFixes++;
            _config.LastFixTime = DateTime.Now;
            _config.Save();
            Log($"Counter-routes applied (total fixes: {_config.TotalFixes})", LogLevel.Info);
            _lastFixTime = DateTime.Now;

            OnFixCompleted?.Invoke();

            if (_config.BarkEnabled && !string.IsNullOrEmpty(_config.BarkServer))
            {
                _ = SendBarkNotification();
            }
        }
        catch (Exception ex)
        {
            Log($"Counter-route error: {ex.Message}", LogLevel.Error);
        }
        finally
        {
            if (_fixing)
            {
                _fixing = false;
                // Only restore to Running if not stopped by user during fix
                if (State == MonitorState.Fixing)
                {
                    State = MonitorState.Running;
                    OnStateChanged?.Invoke(State);
                }
            }
        }
    }

    // --- Helper methods ---

    private static bool IsVirtualAdapter(NetworkInterface nic)
    {
        var desc = nic.Description;
        string[] keywords = { "TAP", "VPN", "WireGuard", "Tunnel", "Virtual", "Hyper-V", "EricVPN" };
        return keywords.Any(k => desc.Contains(k, StringComparison.OrdinalIgnoreCase));
    }

    private List<TapAdapter> GetManagedAdapters()
    {
        string[] managedKeywords = { "TAP", "VPN", "Virtual", "Hyper-V", "EricVPN" };
        string[] ignoreKeywords = { "WireGuard" };

        return NetworkInterface.GetAllNetworkInterfaces()
            .Where(n => n.OperationalStatus == OperationalStatus.Up)
            .Where(n => managedKeywords.Any(k => n.Description.Contains(k, StringComparison.OrdinalIgnoreCase)))
            .Where(n => !ignoreKeywords.Any(k => n.Description.Contains(k, StringComparison.OrdinalIgnoreCase)))
            .Select(n => new TapAdapter
            {
                Name = n.Name,
                IfIndex = n.GetIPProperties().GetIPv4Properties().Index,
                Description = n.Description
            })
            .ToList();
    }

    // --- Route table parsing helpers ---

    /// <summary>
    /// Get cached route table output (cache for 2 seconds to avoid spamming route.exe)
    /// </summary>
    private string GetRouteTable()
    {
        if ((DateTime.Now - _routeTableCacheTime).TotalSeconds < 2 && !string.IsNullOrEmpty(_routeTableCache))
            return _routeTableCache;
        _routeTableCache = RunRoute("print -4");
        _routeTableCacheTime = DateTime.Now;
        return _routeTableCache;
    }

    /// <summary>
    /// Map ifIndex to the interface's IPv4 address (used for matching route print output,
    /// since route print shows interface IP, not ifIndex).
    /// </summary>
    private string? GetInterfaceIp(int ifIndex)
    {
        try
        {
            var nic = NetworkInterface.GetAllNetworkInterfaces()
                .FirstOrDefault(n => n.GetIPProperties().GetIPv4Properties().Index == ifIndex);
            if (nic == null) return null;
            var ip = nic.GetIPProperties().UnicastAddresses
                .FirstOrDefault(a => a.Address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetwork);
            return ip?.Address.ToString();
        }
        catch { return null; }
    }

    /// <summary>
    /// Convert CIDR prefix (e.g. "0.0.0.0/1") to Windows route print format:
    /// ("0.0.0.0", "128.0.0.0") — Network Destination and Netmask.
    /// </summary>
    private static (string dest, string mask) CidrToRoutePrint(string cidr)
    {
        var parts = cidr.Split('/');
        if (parts.Length != 2 || !int.TryParse(parts[1], out int prefixLen) || prefixLen < 0 || prefixLen > 32)
            return (parts[0], "0.0.0.0"); // fallback

        // Build mask in network byte order (big-endian) to avoid IPAddress(uint) endianness issues
        uint maskVal = prefixLen == 0 ? 0 : 0xFFFFFFFFu << (32 - prefixLen);
        byte[] maskBytes = new byte[4];
        maskBytes[0] = (byte)((maskVal >> 24) & 0xFF);
        maskBytes[1] = (byte)((maskVal >> 16) & 0xFF);
        maskBytes[2] = (byte)((maskVal >> 8) & 0xFF);
        maskBytes[3] = (byte)(maskVal & 0xFF);
        var maskAddr = new IPAddress(maskBytes);
        // Network destination = IP AND mask (both in network byte order)
        var ipBytes = IPAddress.Parse(parts[0]).GetAddressBytes();
        var destBytes = new byte[4];
        for (int i = 0; i < 4; i++) destBytes[i] = (byte)(ipBytes[i] & maskBytes[i]);
        return (new IPAddress(destBytes).ToString(), maskAddr.ToString());
    }

    /// <summary>
    /// Check if a CIDR route exists in route table (any interface).
    /// Matches against Windows route print format: Dest  Mask  Gateway  InterfaceIP  Metric
    /// </summary>
    private bool RouteExists(string cidr)
    {
        try
        {
            var output = GetRouteTable();
            var (dest, mask) = CidrToRoutePrint(cidr);
            var lines = output.Split('\n');
            return lines.Any(l =>
            {
                var parts = l.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                return parts.Length >= 5 && parts[0] == dest && parts[1] == mask;
            });
        }
        catch { return false; }
    }

    /// <summary>
    /// Check if a counter-route (CIDR → specific ifIndex) exists in route table.
    /// Uses interface IP mapping since route print shows IP not ifIndex.
    /// </summary>
    private bool CounterRouteExists(string cidr, int ifIndex)
    {
        return RouteExistsOnInterface(cidr, ifIndex);
    }

    /// <summary>
    /// Check if a private network route (CIDR → TAP ifIndex) exists in route table.
    /// </summary>
    private bool PrivateRouteExists(string cidr, int ifIndex)
    {
        return RouteExistsOnInterface(cidr, ifIndex);
    }

    /// <summary>
    /// Check if a CIDR route exists on a specific interface (by ifIndex).
    /// Resolves ifIndex to interface IP for route print matching.
    /// </summary>
    private bool RouteExistsOnInterface(string cidr, int ifIndex)
    {
        try
        {
            var output = GetRouteTable();
            var (dest, mask) = CidrToRoutePrint(cidr);
            var ifIp = GetInterfaceIp(ifIndex);
            var lines = output.Split('\n');
            return lines.Any(l =>
            {
                var parts = l.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 5 || parts[0] != dest || parts[1] != mask)
                    return false;
                // Match by interface IP if available, otherwise match by gateway
                if (!string.IsNullOrEmpty(ifIp))
                    return parts[3] == ifIp;
                return true; // If we can't resolve IP, just match dest+mask
            });
        }
        catch { return false; }
    }

    private string? GetTapGateway(int ifIndex)
    {
        try
        {
            var ifIp = GetInterfaceIp(ifIndex);
            var output = GetRouteTable();
            var lines = output.Split('\n');
            foreach (var line in lines)
            {
                var parts = line.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 5) continue;
                // Match lines on this interface (by interface IP column)
                if (!string.IsNullOrEmpty(ifIp) && parts[3] != ifIp) continue;
                var gw = parts[2];
                if (gw != "On-link" && System.Net.IPAddress.TryParse(gw, out var ip) && !ip.Equals(System.Net.IPAddress.Any))
                    return gw;
            }
        }
        catch { }
        return null;
    }

    private int GetAdapterMetric(int ifIndex)
    {
        try
        {
            var ifIp = GetInterfaceIp(ifIndex);
            var output = GetRouteTable();
            var lines = output.Split('\n');
            foreach (var line in lines)
            {
                var parts = line.Split(new[] { ' ' }, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 5) continue;
                // Look for default route (0.0.0.0/0) on this interface
                if (parts[0] == "0.0.0.0" && parts[1] == "0.0.0.0")
                {
                    if (!string.IsNullOrEmpty(ifIp) && parts[3] != ifIp) continue;
                    if (int.TryParse(parts[4], out var m))
                        return m;
                }
            }
        }
        catch { }
        return -1;
    }

    private string RunRoute(string args)
    {
        try
        {
            var psi = new ProcessStartInfo
            {
                FileName = "route.exe",
                Arguments = args,
                CreateNoWindow = true,
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true
            };
            using var proc = Process.Start(psi);
            if (proc == null) return "";
            var output = proc.StandardOutput.ReadToEnd();
            proc.WaitForExit(5000);
            return output;
        }
        catch { return ""; }
    }

    private async Task SendBarkNotification()
    {
        try
        {
            var server = _config.BarkServer.TrimEnd('/');
            var key = _config.BarkDeviceKey;
            var title = Uri.EscapeDataString(_config.BarkTitle);
            var body = Uri.EscapeDataString($"Route fix #{_config.TotalFixes} at {DateTime.Now:HH:mm:ss}");
            var url = $"{server}/{key}/{title}/{body}";
            if (!string.IsNullOrEmpty(_config.BarkSound))
                url += $"?sound={_config.BarkSound}";

            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
            await http.GetAsync(url);
            Log("Bark notification sent", LogLevel.Info);
        }
        catch (Exception ex)
        {
            Log($"Bark send error: {ex.Message}", LogLevel.Warning);
        }
    }

    private void Log(string message, LogLevel level)
    {
        OnLog?.Invoke(message, level);
    }
}

public class TapAdapter
{
    public string Name { get; set; } = "";
    public int IfIndex { get; set; }
    public string Description { get; set; } = "";
}

public enum LogLevel { Info, Warning, Error }
