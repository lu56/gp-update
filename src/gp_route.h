#pragma once
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <vector>
#include "gp_config.h"
#include "gp_logger.h"

namespace gp {

enum class MonitorState { Stopped, Running, Fixing };

struct TapAdapter {
    std::wstring name;
    int ifIndex = 0;
    std::string description;
};

class RouteEngine {
public:
    RouteEngine(AppConfig& cfg);

    void start();
    void stop();
    void doCheckNow();

    MonitorState getState() const { return state; }

    // Callbacks (called from background thread)
    std::function<void(const std::string&, LogLevel)> onLog;
    std::function<void(MonitorState)> onStateChanged;
    std::function<void()> onFixCompleted;

    // Query current NIC info for UI (called from UI thread)
    std::wstring getMainNicDesc() const { return mainNicDesc; }
    std::wstring getMainNicName() const { return mainNicName; }
    std::wstring getTapNicDesc() const { return tapNicDesc; }
    std::wstring getTapNicName() const { return tapNicName; }

private:
    AppConfig& config;
    std::atomic<MonitorState> state{MonitorState::Stopped};
    std::thread worker;
    std::atomic<bool> running{false};
    std::atomic<bool> fixing{false};

    int mainIfIndex = 0;
    std::string mainGateway;
    std::wstring mainNicName;
    std::wstring mainNicDesc;

    std::wstring tapNicName;
    std::wstring tapNicDesc;

    // Route table cache
    std::string routeCache;
    std::string routeCacheTime; // ISO timestamp
    time_t routeCacheTimeT = 0;

    // Dedup logging
    std::string lastNicLog;
    bool wasHijacked = false;
    bool steadyState = false;

    static const std::vector<std::string> counterRoutes;

    void workerLoop();
    void log(const std::string& msg, LogLevel level = LogLevel::Info);
    void setState(MonitorState s);

    bool cacheMainNic();
    void doCheck();
    void applyCounterRoutes();

    // Helpers
    std::vector<TapAdapter> getManagedAdapters();
    std::string getRouteTable();
    std::string getInterfaceIp(int ifIndex);
    static void cidrToRoutePrint(const std::string& cidr, std::string& dest, std::string& mask);
    bool routeExists(const std::string& cidr);
    bool routeExistsOnInterface(const std::string& cidr, int ifIndex);
    std::string getTapGateway(int ifIndex);
    int getAdapterMetric(int ifIndex);
    static void runRoute(const std::string& args);
    void sendBarkNotification();

    // NIC enumeration via IPHLPAPI
    bool isVirtualAdapterW(const std::wstring& desc) const;
};

} // namespace gp
