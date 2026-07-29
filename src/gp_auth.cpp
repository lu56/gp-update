#include "gp_auth.h"
#include "gp_http.h"
#include "gp_json.h"
#include "gp_logger.h"
#include <ctime>

namespace gp {

AuthResult checkAuth(const std::string& deviceId, const std::string& authServer) {
    AuthResult result;

    // Build JSON body: {"device_id": "..."}
    JsonValue body;
    body.type = JsonValue::Type::Object;
    JsonValue did;
    did.type = JsonValue::Type::String;
    did.strVal = deviceId;
    body.objVal["device_id"] = std::move(did);

    std::string url = authServer + "/api/auth/check";
    Logger::instance().write("[AUTH] checkAuth: POST " + url, LogLevel::Info);
    Logger::instance().write("[AUTH] checkAuth: device_id=" + deviceId, LogLevel::Info);
    Logger::instance().write("[AUTH] checkAuth: calling httpPost...", LogLevel::Info);
    auto resp = httpPost(url, body.dump(), 10);
    Logger::instance().write("[AUTH] checkAuth: httpPost returned, error='" + resp.error +
                             "' statusCode=" + std::to_string(resp.statusCode) +
                             " bodyLen=" + std::to_string(resp.body.size()), LogLevel::Info);

    if (!resp.error.empty()) {
        result.message = "Connection failed: " + resp.error;
        Logger::instance().write("[AUTH] checkAuth: returning error: " + result.message, LogLevel::Warning);
        return result;
    }

    if (resp.statusCode == 0) {
        result.message = "Connection failed";
        Logger::instance().write("[AUTH] checkAuth: statusCode=0, Connection failed", LogLevel::Warning);
        return result;
    }

    Logger::instance().write("[AUTH] checkAuth: parsing JSON response...", LogLevel::Info);
    auto root = JsonValue::parse(resp.body);
    if (!root.isObject()) {
        result.message = "Invalid response";
        return result;
    }

    result.ok = root.has("ok") && root.find("ok")->asBool(false);
    if (root.has("message")) result.message = root.find("message")->asString();
    if (result.ok) {
        auto* data = root.find("data");
        if (data && data->isObject() && data->has("token")) {
            result.token = data->find("token")->asString();
        }
    }

    return result;
}

bool verifyTokenSilent(const std::string& deviceId, AppConfig& config) {
    try {
        Logger::instance().write("[AUTH] verifyTokenSilent: entering try block", LogLevel::Info);
        auto result = checkAuth(deviceId, config.effectiveAuthServer());
        Logger::instance().write("[AUTH] verifyTokenSilent: checkAuth returned ok=" +
                                 std::string(result.ok ? "true" : "false") +
                                 " msg=" + result.message, LogLevel::Info);
        if (result.ok) {
            config.authToken = result.token;
            config.authExpire = makeTokenExpiry();
            config.save();
            return true;
        }

        // Distinguish network errors from auth rejection
        bool networkError = result.message.find("Connection failed") != std::string::npos ||
                            result.message.find("Invalid response") != std::string::npos;
        if (networkError) {
            // Server unreachable - use grace period (trust cached token)
            Logger::instance().write("Server unreachable during re-validation, using cached token (grace period)", LogLevel::Warning);
            if (hasValidTokenCache(config)) {
                return true;
            }
        } else {
            // Auth explicitly rejected by server - clear cache
            Logger::instance().write("Token rejected by server: " + result.message, LogLevel::Warning);
            config.authToken.clear();
            config.authExpire.clear();
            config.save();
        }
        return false;
    } catch (const std::exception& ex) {
        Logger::instance().write(std::string("Token re-validation error: ") + ex.what(), LogLevel::Warning);
        // Network error - trust cached token if still within expiry
        if (hasValidTokenCache(config)) {
            return true;
        }
        return false;
    }
}

bool hasValidTokenCache(const AppConfig& config) {
    Logger::instance().write("[AUTH] hasValidTokenCache: token='" + config.authToken +
                             "' expire='" + config.authExpire + "'", LogLevel::Info);
    if (config.authToken.empty() || config.authExpire.empty()) {
        Logger::instance().write("[AUTH] hasValidTokenCache: token or expire is empty", LogLevel::Info);
        return false;
    }
    // Parse "yyyy-MM-dd HH:mm:ss"
    struct tm lt = {};
    int parsed = sscanf_s(config.authExpire.c_str(), "%d-%d-%d %d:%d:%d",
                 &lt.tm_year, &lt.tm_mon, &lt.tm_mday,
                 &lt.tm_hour, &lt.tm_min, &lt.tm_sec);
    Logger::instance().write("[AUTH] hasValidTokenCache: sscanf parsed " + std::to_string(parsed) + " fields", LogLevel::Info);
    if (parsed != 6) return false;
    lt.tm_year -= 1900;
    lt.tm_mon -= 1;
    time_t exp = mktime(&lt);
    time_t now = time(nullptr);
    Logger::instance().write("[AUTH] hasValidTokenCache: exp=" + std::to_string((long long)exp) +
                             " now=" + std::to_string((long long)now), LogLevel::Info);
    if (exp == (time_t)-1) return false;
    return now < exp;
}

std::string makeTokenExpiry() {
    time_t now = time(nullptr) + 2 * 24 * 3600; // 2 days
    struct tm lt;
    localtime_s(&lt, &now);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
    return std::string(buf);
}

} // namespace gp
