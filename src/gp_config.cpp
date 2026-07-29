#include "gp_config.h"
#include "gp_json.h"
#include <windows.h>
#include <fstream>
#include <sstream>

namespace gp {

std::string AppConfig::baseDir;
std::string AppConfig::configPath;

static std::string getExeDir() {
    char path[MAX_PATH] = {0};
    GetModuleFileNameA(nullptr, path, MAX_PATH);
    std::string p(path);
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) return p.substr(0, pos);
    return ".";
}

AppConfig AppConfig::load() {
    baseDir = getExeDir();
    configPath = baseDir + "\\config.json";

    AppConfig cfg;

    std::ifstream f(configPath);
    if (!f.is_open()) return cfg;

    std::stringstream ss;
    ss << f.rdbuf();
    std::string content = ss.str();

    JsonValue root = JsonValue::parse(content);
    if (!root.isObject()) return cfg;

    if (auto* v = root.find("AppVersion")) cfg.appVersion = v->asString("2.0.0");
    if (auto* v = root.find("CheckIntervalSeconds")) cfg.checkIntervalSeconds = v->asInt(4);
    if (auto* v = root.find("TapMetric")) cfg.tapMetric = v->asInt(50);
    if (auto* v = root.find("MainMetric")) cfg.mainMetric = v->asInt(16);
    if (auto* v = root.find("AutoStart")) cfg.autoStart = v->asBool(true);
    if (auto* v = root.find("MinimizeToTray")) cfg.minimizeToTray = v->asBool(true);
    if (auto* v = root.find("ShowNotification")) cfg.showNotification = v->asBool(true);
    if (auto* v = root.find("BarkEnabled")) cfg.barkEnabled = v->asBool(false);
    if (auto* v = root.find("BarkServer")) cfg.barkServer = v->asString();
    if (auto* v = root.find("BarkDeviceKey")) cfg.barkDeviceKey = v->asString();
    if (auto* v = root.find("BarkTitle")) cfg.barkTitle = v->asString("GatewayPolicy");
    if (auto* v = root.find("BarkSound")) cfg.barkSound = v->asString();
    if (auto* v = root.find("AutoUpdateCheck")) cfg.autoUpdateCheck = v->asBool(true);
    if (auto* v = root.find("UpdateRepo")) cfg.updateRepo = v->asString();
    if (auto* v = root.find("AuthServer")) cfg.authServer = v->asString();
    if (auto* v = root.find("AuthToken")) cfg.authToken = v->asString();
    if (auto* v = root.find("AuthExpire")) cfg.authExpire = v->asString();
    if (auto* v = root.find("TotalFixes")) cfg.totalFixes = v->asInt64(0);
    if (auto* v = root.find("LastFixTime")) cfg.lastFixTime = v->asString();

    if (auto* arr = root.find("PrivateNets"); arr && arr->isArray()) {
        cfg.privateNets.clear();
        for (auto& item : arr->arrVal) {
            if (item.isString()) cfg.privateNets.push_back(item.strVal);
        }
    }

    if (auto* obj = root.find("CustomRoutes"); obj && obj->isObject()) {
        cfg.customRoutes.clear();
        for (auto& [k, v] : obj->objVal) {
            if (v.isString()) cfg.customRoutes[k] = v.strVal;
        }
    }

    return cfg;
}

void AppConfig::save() const {
    JsonValue root;
    root.type = JsonValue::Type::Object;

    root.objVal["AppVersion"] = JsonValue();
    root.objVal["AppVersion"].type = JsonValue::Type::String;
    root.objVal["AppVersion"].strVal = appVersion;

    root.objVal["CheckIntervalSeconds"] = JsonValue();
    root.objVal["CheckIntervalSeconds"].type = JsonValue::Type::Number;
    root.objVal["CheckIntervalSeconds"].numVal = checkIntervalSeconds;

    root.objVal["TapMetric"] = JsonValue();
    root.objVal["TapMetric"].type = JsonValue::Type::Number;
    root.objVal["TapMetric"].numVal = tapMetric;

    root.objVal["MainMetric"] = JsonValue();
    root.objVal["MainMetric"].type = JsonValue::Type::Number;
    root.objVal["MainMetric"].numVal = mainMetric;

    root.objVal["AutoStart"] = JsonValue();
    root.objVal["AutoStart"].type = JsonValue::Type::Bool;
    root.objVal["AutoStart"].boolVal = autoStart;

    root.objVal["MinimizeToTray"] = JsonValue();
    root.objVal["MinimizeToTray"].type = JsonValue::Type::Bool;
    root.objVal["MinimizeToTray"].boolVal = minimizeToTray;

    root.objVal["ShowNotification"] = JsonValue();
    root.objVal["ShowNotification"].type = JsonValue::Type::Bool;
    root.objVal["ShowNotification"].boolVal = showNotification;

    root.objVal["BarkEnabled"] = JsonValue();
    root.objVal["BarkEnabled"].type = JsonValue::Type::Bool;
    root.objVal["BarkEnabled"].boolVal = barkEnabled;

    root.objVal["BarkServer"] = JsonValue();
    root.objVal["BarkServer"].type = JsonValue::Type::String;
    root.objVal["BarkServer"].strVal = barkServer;

    root.objVal["BarkDeviceKey"] = JsonValue();
    root.objVal["BarkDeviceKey"].type = JsonValue::Type::String;
    root.objVal["BarkDeviceKey"].strVal = barkDeviceKey;

    root.objVal["BarkTitle"] = JsonValue();
    root.objVal["BarkTitle"].type = JsonValue::Type::String;
    root.objVal["BarkTitle"].strVal = barkTitle;

    root.objVal["BarkSound"] = JsonValue();
    root.objVal["BarkSound"].type = JsonValue::Type::String;
    root.objVal["BarkSound"].strVal = barkSound;

    root.objVal["AutoUpdateCheck"] = JsonValue();
    root.objVal["AutoUpdateCheck"].type = JsonValue::Type::Bool;
    root.objVal["AutoUpdateCheck"].boolVal = autoUpdateCheck;

    root.objVal["UpdateRepo"] = JsonValue();
    root.objVal["UpdateRepo"].type = JsonValue::Type::String;
    root.objVal["UpdateRepo"].strVal = updateRepo;

    root.objVal["AuthServer"] = JsonValue();
    root.objVal["AuthServer"].type = JsonValue::Type::String;
    root.objVal["AuthServer"].strVal = authServer;

    root.objVal["AuthToken"] = JsonValue();
    root.objVal["AuthToken"].type = JsonValue::Type::String;
    root.objVal["AuthToken"].strVal = authToken;

    root.objVal["AuthExpire"] = JsonValue();
    root.objVal["AuthExpire"].type = JsonValue::Type::String;
    root.objVal["AuthExpire"].strVal = authExpire;

    root.objVal["TotalFixes"] = JsonValue();
    root.objVal["TotalFixes"].type = JsonValue::Type::Number;
    root.objVal["TotalFixes"].numVal = (double)totalFixes;

    root.objVal["LastFixTime"] = JsonValue();
    root.objVal["LastFixTime"].type = JsonValue::Type::String;
    root.objVal["LastFixTime"].strVal = lastFixTime;

    // PrivateNets array
    JsonValue pn;
    pn.type = JsonValue::Type::Array;
    for (auto& s : privateNets) {
        JsonValue v;
        v.type = JsonValue::Type::String;
        v.strVal = s;
        pn.arrVal.push_back(std::move(v));
    }
    root.objVal["PrivateNets"] = std::move(pn);

    // CustomRoutes object
    JsonValue cr;
    cr.type = JsonValue::Type::Object;
    for (auto& [k, v] : customRoutes) {
        JsonValue val;
        val.type = JsonValue::Type::String;
        val.strVal = v;
        cr.objVal[k] = std::move(val);
    }
    root.objVal["CustomRoutes"] = std::move(cr);

    std::ofstream f(configPath);
    if (f.is_open()) {
        f << root.dump(true);
    }
}

} // namespace gp
