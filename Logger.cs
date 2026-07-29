using System.Text;

namespace GatewayPolicy;

public static class Logger
{
    private static readonly string LogDir = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "logs");
    private static readonly object LockObj = new();
    private static string? _currentLogFile;
    private static readonly int MaxLogFiles = 30;

    public static event Action<string, LogLevel>? OnLog;

    public static void Write(string message, LogLevel level = LogLevel.Info)
    {
        var timestamp = DateTime.Now;
        var prefix = level switch
        {
            LogLevel.Info => "INFO",
            LogLevel.Warning => "WARN",
            LogLevel.Error => "ERR ",
            _ => "INFO"
        };
        var line = $"[{timestamp:HH:mm:ss}] [{prefix}] {message}";

        OnLog?.Invoke(line, level);
        WriteToFile(timestamp, line);
    }

    private static void WriteToFile(DateTime timestamp, string line)
    {
        lock (LockObj)
        {
            try
            {
                Directory.CreateDirectory(LogDir);
                var logFile = Path.Combine(LogDir, $"gp_{timestamp:yyyyMMdd}.log");

                if (_currentLogFile != logFile)
                {
                    _currentLogFile = logFile;
                    CleanOldLogs();
                }

                File.AppendAllText(logFile, line + Environment.NewLine, Encoding.UTF8);
            }
            catch { }
        }
    }

    private static void CleanOldLogs()
    {
        try
        {
            var files = Directory.GetFiles(LogDir, "gp_*.log")
                .OrderByDescending(f => f)
                .Skip(MaxLogFiles);
            foreach (var f in files)
            {
                try { File.Delete(f); } catch { }
            }
        }
        catch { }
    }

    public static string GetRecentLogs(int lines = 500)
    {
        try
        {
            var logFiles = Directory.GetFiles(LogDir, "gp_*.log")
                .OrderByDescending(f => f);
            var sb = new StringBuilder();
            var count = 0;

            foreach (var file in logFiles)
            {
                var content = File.ReadAllText(file);
                var fileLines = content.Split(new[] { Environment.NewLine }, StringSplitOptions.None)
                    .Where(l => !string.IsNullOrWhiteSpace(l)).ToList();

                foreach (var line in fileLines.Reverse<string>())
                {
                    sb.Insert(0, line + Environment.NewLine);
                    if (++count >= lines) break;
                }
                if (count >= lines) break;
            }
            return sb.ToString();
        }
        catch { return ""; }
    }

    public static string ExportAllLogs()
    {
        try
        {
            var sb = new StringBuilder();
            var logFiles = Directory.GetFiles(LogDir, "gp_*.log").OrderBy(f => f);
            foreach (var file in logFiles)
            {
                sb.AppendLine($"=== {Path.GetFileName(file)} ===");
                sb.AppendLine(File.ReadAllText(file));
            }
            return sb.ToString();
        }
        catch { return ""; }
    }

    public static void ClearLogs()
    {
        try
        {
            foreach (var f in Directory.GetFiles(LogDir, "gp_*.log"))
            {
                try { File.Delete(f); } catch { }
            }
        }
        catch { }
    }
}
