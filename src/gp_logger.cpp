#include "gp_logger.h"
#include <windows.h>
#include <ctime>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace gp {

namespace fs = std::filesystem;

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(const std::string& baseDir) {
    std::lock_guard<std::mutex> lock(mtx);
    logDir = baseDir + "\\logs";
    fs::create_directories(logDir);
}

std::string Logger::levelStr(LogLevel l) {
    switch (l) {
        case LogLevel::Info: return "INFO";
        case LogLevel::Warning: return "WARN";
        case LogLevel::Error: return "ERR ";
        default: return "INFO";
    }
}

void Logger::write(const std::string& message, LogLevel level) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);

    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &lt);
    std::string line = "[" + std::string(timeStr) + "] [" + levelStr(level) + "] " + message;

    if (callback) callback(line, level);

    std::lock_guard<std::mutex> lock(mtx);

    char dateStr[16];
    strftime(dateStr, sizeof(dateStr), "%Y%m%d", &lt);
    std::string logFile = logDir + "\\gp_" + std::string(dateStr) + ".log";

    if (currentLogFile != logFile) {
        currentLogFile = logFile;
        cleanOldLogs();
    }

    std::ofstream f(logFile, std::ios::app);
    if (f.is_open()) {
        f << line << "\n";
    }
}

void Logger::cleanOldLogs() {
    namespace fs = std::filesystem;
    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(logDir)) {
        if (e.path().extension() == ".log" &&
            e.path().filename().string().substr(0, 3) == "gp_") {
            files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end(), std::greater<std::string>());

    // Keep only 30 most recent
    for (size_t i = 30; i < files.size(); i++) {
        try { fs::remove(files[i]); } catch (...) {}
    }
}

std::string Logger::getRecentLogs(int maxLines) {
    std::lock_guard<std::mutex> lock(mtx);
    namespace fs = std::filesystem;

    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(logDir)) {
        if (e.path().extension() == ".log" &&
            e.path().filename().string().substr(0, 3) == "gp_") {
            files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end(), std::greater<std::string>());

    std::vector<std::string> allLines;
    for (auto& file : files) {
        std::ifstream f(file);
        std::string line;
        std::vector<std::string> fileLines;
        while (std::getline(f, line)) {
            if (!line.empty()) fileLines.push_back(line);
        }
        // Insert in reverse (most recent first)
        for (auto it = fileLines.rbegin(); it != fileLines.rend(); ++it) {
            allLines.push_back(*it);
            if ((int)allLines.size() >= maxLines) break;
        }
        if ((int)allLines.size() >= maxLines) break;
    }

    // Reverse to chronological order
    std::reverse(allLines.begin(), allLines.end());

    std::ostringstream ss;
    for (auto& l : allLines) {
        ss << l << "\n";
    }
    return ss.str();
}

std::string Logger::exportAllLogs() {
    std::lock_guard<std::mutex> lock(mtx);
    namespace fs = std::filesystem;

    std::vector<std::string> files;
    for (auto& e : fs::directory_iterator(logDir)) {
        if (e.path().extension() == ".log" &&
            e.path().filename().string().substr(0, 3) == "gp_") {
            files.push_back(e.path().string());
        }
    }
    std::sort(files.begin(), files.end());

    std::ostringstream ss;
    for (auto& file : files) {
        ss << "=== " << fs::path(file).filename().string() << " ===\n";
        std::ifstream f(file);
        ss << f.rdbuf();
    }
    return ss.str();
}

void Logger::clearLogs() {
    std::lock_guard<std::mutex> lock(mtx);
    namespace fs = std::filesystem;
    for (auto& e : fs::directory_iterator(logDir)) {
        if (e.path().extension() == ".log") {
            try { fs::remove(e); } catch (...) {}
        }
    }
}

} // namespace gp
