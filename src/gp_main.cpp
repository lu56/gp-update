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
    gp::Logger::instance().write("=== GatewayPolicy v" + config.appVersion + " starting ===", gp::LogLevel::Info);

    // Device fingerprint
    std::string deviceId = gp::generateDeviceFingerprint();
    gp::Logger::instance().write("Device ID: " + deviceId, gp::LogLevel::Info);

    // ===== MANDATORY AUTH =====
    std::string authServer = config.effectiveAuthServer();

    if (!gp::hasValidTokenCache(config)) {
        // No valid cache - must show auth dialog
        if (!gp::showAuthDialog(hInstance, nullptr, deviceId, authServer, config)) {
            gp::Logger::instance().write("Auth failed or cancelled, exiting", gp::LogLevel::Warning);
            return 0;
        }
    } else {
        // Have cached token - verify silently
        if (!gp::verifyTokenSilent(deviceId, config)) {
            // Token revoked or expired - show auth dialog
            if (!gp::showAuthDialog(hInstance, nullptr, deviceId, authServer, config)) {
                gp::Logger::instance().write("Auth failed or cancelled, exiting", gp::LogLevel::Warning);
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
