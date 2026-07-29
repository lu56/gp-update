#include "gp_update.h"
#include "gp_http.h"
#include "gp_json.h"
#include "gp_logger.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace gp {

namespace fs = std::filesystem;

void parseVersion(const std::string& ver, int out[3]) {
    out[0] = out[1] = out[2] = 0;
    std::string v = ver;
    // Strip leading 'v' or 'V'
    while (!v.empty() && (v[0] == 'v' || v[0] == 'V')) v = v.substr(1);

    int idx = 0;
    size_t i = 0;
    while (i < v.size() && idx < 3) {
        std::string num;
        while (i < v.size() && v[i] >= '0' && v[i] <= '9') {
            num += v[i++];
        }
        if (!num.empty()) {
            out[idx++] = std::stoi(num);
        }
        if (i < v.size() && v[i] == '.') i++;
        else break;
    }
}

bool isNewerVersion(const std::string& remote, const std::string& local) {
    int r[3], l[3];
    parseVersion(remote, r);
    parseVersion(local, l);
    if (r[0] != l[0]) return r[0] > l[0];
    if (r[1] != l[1]) return r[1] > l[1];
    return r[2] > l[2];
}

UpdateInfo checkUpdate(const AppConfig& config) {
    UpdateInfo info;

    if (config.updateRepo.empty()) {
        info.error = "Update repo not configured";
        return info;
    }

    std::string url = "https://api.github.com/repos/" + config.updateRepo + "/releases/latest";
    auto resp = httpGet(url, 15);
    if (!resp.error.empty()) {
        info.error = resp.error;
        return info;
    }

    auto root = JsonValue::parse(resp.body);
    if (!root.isObject()) {
        info.error = "Invalid response";
        return info;
    }

    if (!root.has("tag_name")) return info;
    info.tagName = root.find("tag_name")->asString();

    if (info.tagName.empty()) return info;

    // Check if newer
    std::string remoteVer = info.tagName;
    // Strip 'v' prefix
    while (!remoteVer.empty() && (remoteVer[0] == 'v' || remoteVer[0] == 'V')) remoteVer = remoteVer.substr(1);

    if (!isNewerVersion(remoteVer, config.appVersion)) {
        return info; // No update
    }

    info.hasUpdate = true;

    // Find exe asset
    if (root.has("assets") && root.find("assets")->isArray()) {
        for (auto& asset : root.find("assets")->arrVal) {
            if (asset.isObject() && asset.has("name")) {
                std::string name = asset.find("name")->asString();
                // Check .exe extension
                if (name.size() > 4 &&
                    (name.substr(name.size() - 4) == ".exe" || name.substr(name.size() - 4) == ".EXE")) {
                    info.assetName = name;
                    if (asset.has("browser_download_url")) {
                        info.downloadUrl = asset.find("browser_download_url")->asString();
                    }
                    break;
                }
            }
        }
    }

    return info;
}

bool downloadAndInstall(const std::string& downloadUrl, const std::string& fileName) {
    try {
        char tempPath[MAX_PATH];
        GetTempPathA(MAX_PATH, tempPath);
        std::string tempDir = std::string(tempPath) + "GatewayPolicy_Update";
        fs::create_directories(tempDir);
        std::string tempFile = tempDir + "\\" + fileName;

        Logger::instance().write("Downloading update: " + fileName, LogLevel::Info);

        auto resp = httpDownload(downloadUrl, 300);
        if (!resp.error.empty() || resp.statusCode == 0) {
            Logger::instance().write("Download failed: " + resp.error, LogLevel::Error);
            return false;
        }

        std::ofstream f(tempFile, std::ios::binary);
        if (!f.is_open()) return false;
        f.write(resp.body.data(), resp.body.size());
        f.close();

        Logger::instance().write("Download complete: " + tempFile, LogLevel::Info);

        // Create updater BAT script
        char exePath[MAX_PATH] = {0};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string currentExe(exePath);

        std::string batPath = tempDir + "\\updater.bat";
        std::ostringstream script;
        script << "@echo off\r\n"
               << "echo GatewayPolicy Updater\r\n"
               << "timeout /t 2 /nobreak >nul\r\n"
               << ":retry\r\n"
               << "del /f /q \"" << currentExe << "\" 2>nul\r\n"
               << "if exist \"" << currentExe << "\" (\r\n"
               << "    timeout /t 1 /nobreak >nul\r\n"
               << "    goto retry\r\n"
               << ")\r\n"
               << "copy /y \"" << tempFile << "\" \"" << currentExe << "\" >nul\r\n"
               << "start \"\" \"" << currentExe << "\"\r\n"
               << "del /f /q \"%~f0\"\r\n";

        std::ofstream bf(batPath, std::ios::binary);
        bf << script.str();
        bf.close();

        // Run updater
        STARTUPINFOA si = {sizeof(si)};
        PROCESS_INFORMATION pi = {};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        if (CreateProcessA(nullptr, const_cast<char*>(batPath.c_str()), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }

        return true;
    } catch (const std::exception& ex) {
        Logger::instance().write(std::string("Update error: ") + ex.what(), LogLevel::Error);
        return false;
    }
}

} // namespace gp
