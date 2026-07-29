#pragma once
#include <string>
#include <vector>
#include <map>

namespace gp {

struct AppConfig {
    // Hardcoded auth server
    static constexpr const char* DefaultAuthServer = "https://pve.lu56.top:12233";

    std::string appVersion = "2.0.0";

    // Monitor
    int checkIntervalSeconds = 4;
    int tapMetric = 50;
    int mainMetric = 16;

    // Private networks
    std::vector<std::string> privateNets = {"10.0.0.0/8", "172.16.0.0/12", "192.168.0.0/16", "100.64.0.0/10"};

    // Custom routes: prefix -> "tap" or "main"
    std::map<std::string, std::string> customRoutes;

    // Behavior
    bool autoStart = true;
    bool minimizeToTray = true;
    bool showNotification = true;

    // Bark push
    bool barkEnabled = false;
    std::string barkServer;
    std::string barkDeviceKey;
    std::string barkTitle = "GatewayPolicy";
    std::string barkSound;

    // Auto update
    bool autoUpdateCheck = true;
    std::string updateRepo;

    // Auth
    std::string authServer; // override only if non-empty
    std::string authToken;
    std::string authExpire;

    // State
    long long totalFixes = 0;
    std::string lastFixTime;

    // Config file path
    static std::string configPath;
    static std::string baseDir;

    static AppConfig load();
    void save() const;

    std::string effectiveAuthServer() const {
        return !authServer.empty() ? authServer : DefaultAuthServer;
    }
};

} // namespace gp
