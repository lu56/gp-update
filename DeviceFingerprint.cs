using System.Management;
using System.Net.NetworkInformation;
using System.Security.Cryptography;
using System.Text;

namespace GatewayPolicy;

public static class DeviceFingerprint
{
    /// <summary>
    /// Generate a stable device fingerprint from CPU ID + Disk Serial + MAC.
    /// Returns a 32-char hex string (SHA256 truncated).
    /// </summary>
    public static string Generate()
    {
        var parts = new List<string>();

        // CPU ProcessorId
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT ProcessorId FROM Win32_Processor");
            foreach (ManagementObject obj in searcher.Get())
            {
                var id = obj["ProcessorId"]?.ToString();
                if (!string.IsNullOrEmpty(id)) parts.Add(id);
                break;
            }
        }
        catch { }

        // System disk serial number
        try
        {
            using var searcher = new ManagementObjectSearcher("SELECT SerialNumber FROM Win32_DiskDrive WHERE Index=0");
            foreach (ManagementObject obj in searcher.Get())
            {
                var sn = obj["SerialNumber"]?.ToString()?.Trim();
                if (!string.IsNullOrEmpty(sn)) parts.Add(sn);
                break;
            }
        }
        catch { }

        // Primary NIC MAC
        try
        {
            var nic = NetworkInterface.GetAllNetworkInterfaces()
                .Where(n => n.OperationalStatus == OperationalStatus.Up &&
                            n.NetworkInterfaceType != NetworkInterfaceType.Loopback &&
                            !IsVirtual(n))
                .FirstOrDefault();
            if (nic != null)
                parts.Add(nic.GetPhysicalAddress().ToString());
        }
        catch { }

        if (parts.Count == 0) return "unknown";

        var raw = string.Join("|", parts);
        using var sha = SHA256.Create();
        var hash = sha.ComputeHash(Encoding.UTF8.GetBytes(raw));
        return Convert.ToHexString(hash).Substring(0, 32).ToLower();
    }

    private static bool IsVirtual(NetworkInterface nic)
    {
        var desc = nic.Description;
        string[] keywords = { "TAP", "VPN", "WireGuard", "Tunnel", "Virtual", "Hyper-V", "EricVPN" };
        return keywords.Any(k => desc.Contains(k, StringComparison.OrdinalIgnoreCase));
    }
}
