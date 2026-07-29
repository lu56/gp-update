#include "gp_route.h"
#include "gp_http.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <ctime>
#include <sstream>
#include <algorithm>
#include <cstring>

// Required for GetAdaptersAddresses gateway info on Windows 10 build 16299+
#ifndef GAA_FLAG_INCLUDE_GATEWAYS
#define GAA_FLAG_INCLUDE_GATEWAYS 0x0080
#endif

namespace gp {

const std::vector<std::string> RouteEngine::counterRoutes = {
    "0.0.0.0/2", "64.0.0.0/2", "128.0.0.0/2", "192.0.0.0/2"
};

RouteEngine::RouteEngine(AppConfig& cfg) : config(cfg) {}

void RouteEngine::log(const std::string& msg, LogLevel level) {
    Logger::instance().write(msg, level);
    if (onLog) onLog(msg, level);
}

void RouteEngine::setState(MonitorState s) {
    state = s;
    if (onStateChanged) onStateChanged(s);
}

void RouteEngine::start() {
    if (state != MonitorState::Stopped) return;

    running = true;
    cacheMainNic();
    setState(MonitorState::Running);
    log("Monitor started (v2.0: /2 counter-route strategy, C++ edition)", LogLevel::Info);

    worker = std::thread([this]() { workerLoop(); });
}

void RouteEngine::stop() {
    running = false;
    fixing = false;
    if (worker.joinable()) worker.detach();
    setState(MonitorState::Stopped);
    log("Monitor stopped", LogLevel::Info);
}

void RouteEngine::doCheckNow() {
    if (fixing) return;
    doCheck();
}

void RouteEngine::workerLoop() {
    // Initial delay 2 seconds
    Sleep(2000);
    while (running) {
        if (!fixing) {
            try { doCheck(); } catch (...) {}
        }
        int interval = config.checkIntervalSeconds * 1000;
        if (interval < 1000) interval = 1000;
        // Sleep in small chunks for responsive shutdown
        for (int i = 0; i < interval && running; i += 200) {
            Sleep(std::min(200, interval - i));
        }
    }
}

// ===== NIC detection =====

static std::string wstrToUtf8(const std::wstring& ws) {
    if (ws.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(len - 1, 0);
    WideCharToMultiByte(CP_UTF8, 0, ws.c_str(), -1, &s[0], len, nullptr, nullptr);
    return s;
}

static std::wstring utf8ToWstr(const std::string& s) {
    if (s.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring ws(len - 1, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &ws[0], len);
    return ws;
}

bool RouteEngine::isVirtualAdapterW(const std::wstring& desc) const {
    std::wstring lower = desc;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const wchar_t* keywords[] = {
        L"tap", L"vpn", L"wireguard", L"tunnel", L"virtual", L"hyper-v", L"ericvpn"
    };
    for (auto kw : keywords) {
        if (lower.find(kw) != std::wstring::npos) return true;
    }
    return false;
}

bool RouteEngine::cacheMainNic() {
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);

    DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    DWORD dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                       reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    if (dwRet == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    }
    if (dwRet != NO_ERROR) return false;

    auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    for (; pAddr; pAddr = pAddr->Next) {
        if (pAddr->OperStatus != IfOperStatusUp) continue;
        if (pAddr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (isVirtualAdapterW(pAddr->Description)) continue;

        // Check for IPv4 gateway
        for (auto gw = pAddr->FirstGatewayAddress; gw; gw = gw->Next) {
            if (gw->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto sin = reinterpret_cast<sockaddr_in*>(gw->Address.lpSockaddr);
            char ipStr[32];
            inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
            std::string gwStr(ipStr);
            if (gwStr.empty() || gwStr == "0.0.0.0") continue;

            mainIfIndex = pAddr->IfIndex;
            mainGateway = gwStr;
            mainNicName = pAddr->FriendlyName ? pAddr->FriendlyName : L"";
            mainNicDesc = pAddr->Description ? pAddr->Description : L"";

            std::string nicInfo = wstrToUtf8(mainNicName) + "|" + std::to_string(mainIfIndex) + "|" + mainGateway;
            if (lastNicLog != nicInfo) {
                lastNicLog = nicInfo;
                log("Main NIC: " + wstrToUtf8(mainNicName) + " (if=" + std::to_string(mainIfIndex) +
                    ", gw=" + mainGateway + ")", LogLevel::Info);
            }
            return true;
        }
    }
    return false;
}

std::vector<TapAdapter> RouteEngine::getManagedAdapters() {
    std::vector<TapAdapter> result;
    const wchar_t* managedKw[] = {L"tap", L"vpn", L"virtual", L"hyper-v", L"ericvpn"};
    const wchar_t* ignoreKw[] = {L"wireguard"};

    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                       reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    if (dwRet == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    }
    if (dwRet != NO_ERROR) return result;

    auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    for (; pAddr; pAddr = pAddr->Next) {
        if (pAddr->OperStatus != IfOperStatusUp) continue;

        std::wstring desc = pAddr->Description ? pAddr->Description : L"";
        std::wstring lowerDesc = desc;
        std::transform(lowerDesc.begin(), lowerDesc.end(), lowerDesc.begin(), ::towlower);

        bool managed = false;
        for (auto kw : managedKw) {
            if (lowerDesc.find(kw) != std::wstring::npos) { managed = true; break; }
        }
        if (!managed) continue;

        bool ignored = false;
        for (auto kw : ignoreKw) {
            if (lowerDesc.find(kw) != std::wstring::npos) { ignored = true; break; }
        }
        if (ignored) continue;

        TapAdapter tap;
        tap.name = pAddr->FriendlyName ? pAddr->FriendlyName : L"";
        tap.ifIndex = pAddr->IfIndex;
        tap.description = wstrToUtf8(desc);
        result.push_back(tap);

        // Update tapNicDesc/Name for UI
        tapNicName = tap.name;
        tapNicDesc = desc;
    }
    if (result.empty()) {
        tapNicName.clear();
        tapNicDesc.clear();
    }
    return result;
}

// ===== Route table helpers =====

static std::string runRouteExe(const std::string& args) {
    std::string result;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};

    std::string cmd = "route.exe " + args;
    if (CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        char buf[8192];
        DWORD read = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
            result.append(buf, read);
        }
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    CloseHandle(hWrite);
    return result;
}

std::string RouteEngine::getRouteTable() {
    time_t now = time(nullptr);
    if (!routeCache.empty() && (now - routeCacheTimeT) < 2) {
        return routeCache;
    }
    routeCache = runRouteExe("print -4");
    routeCacheTimeT = now;
    return routeCache;
}

std::string RouteEngine::getInterfaceIp(int ifIndex) {
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);
    DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                       reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    if (dwRet == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    }
    if (dwRet != NO_ERROR) return "";

    auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    for (; pAddr; pAddr = pAddr->Next) {
        if ((int)pAddr->IfIndex != ifIndex) continue;
        for (auto ua = pAddr->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            char ipStr[32];
            inet_ntop(AF_INET, &sin->sin_addr, ipStr, sizeof(ipStr));
            return std::string(ipStr);
        }
    }
    return "";
}

void RouteEngine::cidrToRoutePrint(const std::string& cidr, std::string& dest, std::string& mask) {
    auto slash = cidr.find('/');
    if (slash == std::string::npos) { dest = cidr; mask = "0.0.0.0"; return; }

    std::string ipStr = cidr.substr(0, slash);
    int prefixLen = atoi(cidr.substr(slash + 1).c_str());
    if (prefixLen < 0 || prefixLen > 32) { dest = ipStr; mask = "0.0.0.0"; return; }

    // Build mask (big-endian / network byte order)
    uint32_t maskVal = (prefixLen == 0) ? 0 : (0xFFFFFFFFu << (32 - prefixLen));
    unsigned char maskBytes[4] = {
        (unsigned char)((maskVal >> 24) & 0xFF),
        (unsigned char)((maskVal >> 16) & 0xFF),
        (unsigned char)((maskVal >> 8) & 0xFF),
        (unsigned char)(maskVal & 0xFF)
    };

    // Parse IP string to bytes
    unsigned char ipBytes[4] = {0};
    int parts[4] = {0};
    int n = sscanf(ipStr.c_str(), "%d.%d.%d.%d", &parts[0], &parts[1], &parts[2], &parts[3]);
    if (n == 4) {
        for (int i = 0; i < 4; i++) ipBytes[i] = (unsigned char)parts[i];
    }

    // dest = IP AND mask
    unsigned char destBytes[4];
    for (int i = 0; i < 4; i++) destBytes[i] = ipBytes[i] & maskBytes[i];

    char destBuf[20], maskBuf[20];
    snprintf(destBuf, sizeof(destBuf), "%d.%d.%d.%d", destBytes[0], destBytes[1], destBytes[2], destBytes[3]);
    snprintf(maskBuf, sizeof(maskBuf), "%d.%d.%d.%d", maskBytes[0], maskBytes[1], maskBytes[2], maskBytes[3]);
    dest = destBuf;
    mask = maskBuf;
}

// Split string by whitespace, return tokens
static std::vector<std::string> splitWs(const std::string& line) {
    std::vector<std::string> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '\n') i++;
        tokens.push_back(line.substr(start, i - start));
    }
    return tokens;
}

bool RouteEngine::routeExists(const std::string& cidr) {
    try {
        std::string output = getRouteTable();
        std::string dest, mask;
        cidrToRoutePrint(cidr, dest, mask);

        std::istringstream ss(output);
        std::string line;
        while (std::getline(ss, line)) {
            auto parts = splitWs(line);
            if (parts.size() >= 5 && parts[0] == dest && parts[1] == mask) {
                return true;
            }
        }
    } catch (...) {}
    return false;
}

bool RouteEngine::routeExistsOnInterface(const std::string& cidr, int ifIndex) {
    try {
        std::string output = getRouteTable();
        std::string dest, mask;
        cidrToRoutePrint(cidr, dest, mask);
        std::string ifIp = getInterfaceIp(ifIndex);

        std::istringstream ss(output);
        std::string line;
        while (std::getline(ss, line)) {
            auto parts = splitWs(line);
            if (parts.size() < 5 || parts[0] != dest || parts[1] != mask) continue;
            if (!ifIp.empty()) {
                return parts[3] == ifIp;
            }
            return true;
        }
    } catch (...) {}
    return false;
}

std::string RouteEngine::getTapGateway(int ifIndex) {
    try {
        std::string ifIp = getInterfaceIp(ifIndex);
        std::string output = getRouteTable();

        std::istringstream ss(output);
        std::string line;
        while (std::getline(ss, line)) {
            auto parts = splitWs(line);
            if (parts.size() < 5) continue;
            if (!ifIp.empty() && parts[3] != ifIp) continue;
            std::string gw = parts[2];
            if (gw == "On-link") continue;
            // Validate it's an IP address and not 0.0.0.0
            unsigned int a, b, c, d;
            if (sscanf(gw.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) == 4 &&
                !(a == 0 && b == 0 && c == 0 && d == 0)) {
                return gw;
            }
        }
    } catch (...) {}
    return "";
}

int RouteEngine::getAdapterMetric(int ifIndex) {
    try {
        std::string ifIp = getInterfaceIp(ifIndex);
        std::string output = getRouteTable();

        std::istringstream ss(output);
        std::string line;
        while (std::getline(ss, line)) {
            auto parts = splitWs(line);
            if (parts.size() < 5) continue;
            if (parts[0] != "0.0.0.0" || parts[1] != "0.0.0.0") continue;
            if (!ifIp.empty() && parts[3] != ifIp) continue;
            return atoi(parts[4].c_str());
        }
    } catch (...) {}
    return -1;
}

void RouteEngine::runRoute(const std::string& args) {
    runRouteExe(args);
}

// ===== Main check logic =====

void RouteEngine::doCheck() {
    if (fixing) return;

    try {
        cacheMainNic();

        bool hijacked = false;

        // Primary check: def1 hijack routes
        if (routeExists("0.0.0.0/1") || routeExists("128.0.0.0/1")) {
            hijacked = true;
            if (!wasHijacked) {
                log("Detected VPN def1 hijack route (0.0.0.0/1 or 128.0.0.0/1)", LogLevel::Info);
                wasHijacked = true;
            }
        } else {
            if (wasHijacked) {
                log("VPN hijack cleared - no longer detected", LogLevel::Info);
                wasHijacked = false;
                steadyState = false;
            }
        }

        // Secondary check: TAP has default route with very low metric
        auto taps = getManagedAdapters();
        for (auto& tap : taps) {
            int metric = getAdapterMetric(tap.ifIndex);
            if (metric > 0 && metric < 10) {
                hijacked = true;
                if (!wasHijacked) {
                    log("TAP adapter " + tap.description + " has low metric default route (metric=" +
                        std::to_string(metric) + ")", LogLevel::Info);
                    wasHijacked = true;
                }
            }
        }

        if (hijacked) applyCounterRoutes();
    } catch (const std::exception& ex) {
        log(std::string("Check error: ") + ex.what(), LogLevel::Error);
    }
}

void RouteEngine::applyCounterRoutes() {
    // Phase 1: Check what's missing (no state change)
    bool needFix = false;

    struct RouteToAdd {
        std::string cidr, gateway, label;
        int ifIndex, metric;
    };
    std::vector<RouteToAdd> routesToAdd;

    try {
        // Step 1: /2 counter-routes -> main NIC
        if (!mainGateway.empty() && mainIfIndex > 0) {
            for (auto& prefix : counterRoutes) {
                if (!routeExistsOnInterface(prefix, mainIfIndex)) {
                    routesToAdd.push_back({prefix, mainGateway, "Counter-route", mainIfIndex, config.mainMetric});
                    needFix = true;
                }
            }
        }

        // Step 2: Private networks -> TAP
        auto managedTaps = getManagedAdapters();
        if (!managedTaps.empty()) {
            int tapIf = managedTaps[0].ifIndex;
            std::string tapGw = getTapGateway(tapIf);

            if (!tapGw.empty()) {
                for (auto& net : config.privateNets) {
                    if (!routeExistsOnInterface(net, tapIf)) {
                        routesToAdd.push_back({net, tapGw, "Private route", tapIf, 20});
                        needFix = true;
                    }
                }
            }
        }

        // Step 3: Custom routes
        if (!mainGateway.empty() && !managedTaps.empty()) {
            int tapIf = managedTaps[0].ifIndex;
            std::string tapGw = getTapGateway(tapIf);

            for (auto& [prefix, via] : config.customRoutes) {
                if (via == "tap" && !tapGw.empty()) {
                    if (!routeExistsOnInterface(prefix, tapIf)) {
                        routesToAdd.push_back({prefix, tapGw, "Custom route", tapIf, 20});
                        needFix = true;
                    }
                } else if (via == "main") {
                    if (!routeExistsOnInterface(prefix, mainIfIndex)) {
                        routesToAdd.push_back({prefix, mainGateway, "Custom route", mainIfIndex, config.mainMetric});
                        needFix = true;
                    }
                }
            }
        }

        if (!needFix) {
            if (!steadyState) {
                log("All routes verified OK - monitoring (steady state)", LogLevel::Info);
                steadyState = true;
            }
            return;
        }

        fixing = true;
        setState(MonitorState::Fixing);
        steadyState = false;

        // Phase 2: Actually add routes
        for (auto& r : routesToAdd) {
            std::string cmd = "add " + r.cidr + " " + r.gateway + " if " +
                std::to_string(r.ifIndex) + " metric " + std::to_string(r.metric);
            runRoute(cmd);
            log(r.label + " ADD: " + r.cidr + " -> if=" + std::to_string(r.ifIndex) +
                " gw=" + r.gateway + " metric=" + std::to_string(r.metric), LogLevel::Info);
        }

        config.totalFixes++;

        // Format current time
        time_t now = time(nullptr);
        struct tm lt;
        localtime_s(&lt, &now);
        char timeBuf[32];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &lt);
        config.lastFixTime = timeBuf;
        config.save();

        log("Counter-routes applied (total fixes: " + std::to_string(config.totalFixes) + ")", LogLevel::Info);

        if (onFixCompleted) onFixCompleted();

        if (config.barkEnabled && !config.barkServer.empty()) {
            sendBarkNotification();
        }
    } catch (const std::exception& ex) {
        log(std::string("Counter-route error: ") + ex.what(), LogLevel::Error);
    }

    // finally block - restore state
    if (fixing) {
        fixing = false;
        if (state == MonitorState::Fixing) {
            if (running) {
                setState(MonitorState::Running);
            } else {
                setState(MonitorState::Stopped);
            }
        }
    }
}

void RouteEngine::sendBarkNotification() {
    try {
        std::string server = config.barkServer;
        // trim trailing /
        while (!server.empty() && server.back() == '/') server.pop_back();
        std::string key = config.barkDeviceKey;
        std::string title = urlEncode(config.barkTitle);
        time_t now = time(nullptr);
        struct tm lt;
        localtime_s(&lt, &now);
        char timeBuf[16];
        strftime(timeBuf, sizeof(timeBuf), "%H:%M:%S", &lt);
        std::string body = urlEncode("Route fix #" + std::to_string(config.totalFixes) + " at " + timeBuf);
        std::string url = server + "/" + key + "/" + title + "/" + body;
        if (!config.barkSound.empty()) {
            url += "?sound=" + config.barkSound;
        }
        httpGet(url, 5);
        log("Bark notification sent", LogLevel::Info);
    } catch (const std::exception& ex) {
        log(std::string("Bark send error: ") + ex.what(), LogLevel::Warning);
    }
}

} // namespace gp
