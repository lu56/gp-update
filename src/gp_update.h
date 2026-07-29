#pragma once
#include <string>
#include "gp_config.h"

namespace gp {

struct UpdateInfo {
    bool hasUpdate = false;
    std::string tagName;
    std::string downloadUrl;
    std::string assetName;
    std::string error;
};

// Check for updates from GitHub releases
UpdateInfo checkUpdate(const AppConfig& config);

// Parse version string to {major, minor, patch}
void parseVersion(const std::string& ver, int out[3]);

// Returns true if remote is strictly newer than local
bool isNewerVersion(const std::string& remote, const std::string& local);

// Download and install update (returns true on success)
bool downloadAndInstall(const std::string& downloadUrl, const std::string& fileName);

} // namespace gp
