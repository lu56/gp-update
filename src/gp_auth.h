#pragma once
#include <string>
#include "gp_config.h"

namespace gp {

struct AuthResult {
    bool ok = false;
    std::string message;
    std::string token;
};

// POST to auth server /api/auth/check with device_id
AuthResult checkAuth(const std::string& deviceId, const std::string& authServer);

// Verify cached token silently (returns true if still valid)
bool verifyTokenSilent(const std::string& deviceId, AppConfig& config);

// Check if cached token is within expiry
bool hasValidTokenCache(const AppConfig& config);

// Refresh token expiry (2 days from now)
std::string makeTokenExpiry();

} // namespace gp
