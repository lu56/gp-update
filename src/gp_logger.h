#pragma once
#include <string>
#include <mutex>
#include <functional>

namespace gp {

enum class LogLevel { Info, Warning, Error };

class Logger {
public:
    static Logger& instance();

    void init(const std::string& baseDir);
    void write(const std::string& message, LogLevel level = LogLevel::Info);
    void setCallback(std::function<void(const std::string&, LogLevel)> cb) { callback = cb; }

    std::string getRecentLogs(int lines = 500);
    std::string exportAllLogs();
    void clearLogs();

private:
    Logger() = default;
    std::string logDir;
    std::string currentLogFile;
    std::mutex mtx;
    std::function<void(const std::string&, LogLevel)> callback;

    void cleanOldLogs();
    static std::string levelStr(LogLevel l);
};

} // namespace gp
