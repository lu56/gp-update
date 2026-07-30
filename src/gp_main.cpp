#include <windows.h>
#include <commctrl.h>
#include "gp_config.h"
#include "gp_logger.h"
#include "gp_fingerprint.h"
#include "gp_auth.h"
#include "gp_route.h"
#include "gp_gui.h"

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
    // Initialize common controls
    INITCOMMONCONTROLSEX icc = {sizeof(icc), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    // Load config
    auto config = gp::AppConfig::load();

    // Init logger
    gp::Logger::instance().init(gp::AppConfig::baseDir);
    gp::Logger::instance().write("=== GatewayPolicy v" + config.appVersion + " ===", gp::LogLevel::Info);

    // Device fingerprint
    std::string deviceId = gp::generateDeviceFingerprint();

    // ===== MANDATORY AUTH =====
    std::string authServer = config.effectiveAuthServer();

    bool tokenValid = gp::hasValidTokenCache(config);

    if (!tokenValid) {
        if (!gp::showAuthDialog(hInstance, nullptr, deviceId, authServer, config)) {
            gp::Logger::instance().write("Auth cancelled, exiting", gp::LogLevel::Warning);
            return 0;
        }
    } else {
        if (!gp::verifyTokenSilent(deviceId, config)) {
            if (!gp::showAuthDialog(hInstance, nullptr, deviceId, authServer, config)) {
                gp::Logger::instance().write("Auth cancelled, exiting", gp::LogLevel::Warning);
                return 0;
            }
        }
    }

    gp::Logger::instance().write("Auth OK, starting application", gp::LogLevel::Info);

    // Create route engine
    gp::RouteEngine engine(config);

    // Create and run GUI
    gp::GuiApp app(config, engine, deviceId);
    int ret = app.run();

    gp::Logger::instance().write("=== GatewayPolicy exiting ===", gp::LogLevel::Info);
    return ret;
}
