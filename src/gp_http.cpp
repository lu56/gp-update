#include "gp_http.h"
#include "gp_logger.h"
#include <windows.h>
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>

namespace gp {

// Find curl.exe on the system. Try System32 first (Windows 10+).
static std::string findCurlExe() {
    // Try common locations
    const char* paths[] = {
        "C:\\Windows\\System32\\curl.exe",
        "C:\\Windows\\SysWOW64\\curl.exe",
        nullptr
    };
    for (int i = 0; paths[i]; i++) {
        DWORD attr = GetFileAttributesA(paths[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return std::string(paths[i]);
        }
    }
    // Fallback: just use "curl.exe" (hope it's in PATH)
    return std::string("curl.exe");
}

// Run curl.exe, capture stdout. No stdin (use file for POST body).
static std::string runCurlExe(const std::string& args, int timeoutMs, DWORD* exitCode = nullptr) {
    std::string result;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hStdoutR = nullptr, hStdoutW = nullptr;
    CreatePipe(&hStdoutR, &hStdoutW, &sa, 0);
    // Parent's read end must not be inherited
    SetHandleInformation(hStdoutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = nullptr;  // No stdin for GUI app
    si.hStdOutput = hStdoutW;
    si.hStdError = hStdoutW;

    PROCESS_INFORMATION pi = {};
    std::string curlPath = findCurlExe();
    std::string cmd = curlPath + " " + args;

    bool ok = CreateProcessA(nullptr, const_cast<char*>(cmd.c_str()), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) {
        DWORD err = GetLastError();
        if (exitCode) *exitCode = (DWORD)-1;
        CloseHandle(hStdoutR);
        CloseHandle(hStdoutW);
        return "ERROR: curl.exe not found (CreateProcess failed, err=" + std::to_string(err) + ")";
    }

    // Close child's write end in parent
    CloseHandle(hStdoutW);

    // Read stdout with timeout using PeekNamedPipe
    DWORD startTime = GetTickCount();
    char buf[8192];
    DWORD bytesRead = 0;

    while (true) {
        // Check if process has exited
        DWORD exitCd = 0;
        if (GetExitCodeProcess(pi.hProcess, &exitCd) && exitCd != STILL_ACTIVE) {
            // Process exited, read ALL remaining data (loop until pipe empty)
            DWORD available = 0;
            while (PeekNamedPipe(hStdoutR, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
                DWORD toRead = sizeof(buf) < available ? sizeof(buf) : available;
                if (ReadFile(hStdoutR, buf, toRead, &bytesRead, nullptr) && bytesRead > 0) {
                    result.append(buf, bytesRead);
                } else {
                    break;
                }
            }
            if (exitCode) *exitCode = exitCd;
            break;
        }

        // Check for data
        DWORD available = 0;
        if (PeekNamedPipe(hStdoutR, nullptr, 0, nullptr, &available, nullptr) && available > 0) {
            DWORD toRead = sizeof(buf) < available ? sizeof(buf) : available;
            if (ReadFile(hStdoutR, buf, toRead, &bytesRead, nullptr) && bytesRead > 0) {
                result.append(buf, bytesRead);
            }
        } else {
            // No data yet, sleep briefly
            Sleep(50);
        }

        // Check timeout
        DWORD elapsed = GetTickCount() - startTime;
        if (elapsed > (DWORD)timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            if (exitCode) *exitCode = 1;
            result = "ERROR: curl timeout";
            break;
        }
    }

    // Ensure process is fully terminated
    WaitForSingleObject(pi.hProcess, 2000);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hStdoutR);

    return result;
}

// Parse curl -w output: body + "\n" + status_code
static void parseCurlOutput(const std::string& output, HttpResponse& resp) {
    if (output.empty()) {
        resp.error = "No response from curl";
        return;
    }

    // Find the last newline - everything after it is the status code
    auto lastNl = output.rfind('\n');
    if (lastNl != std::string::npos && lastNl < output.size() - 1) {
        std::string statusStr = output.substr(lastNl + 1);
        // Trim whitespace/carriage returns
        while (!statusStr.empty() && (statusStr.back() == '\r' || statusStr.back() == '\n' ||
               statusStr.back() == ' ' || statusStr.back() == '\t')) {
            statusStr.pop_back();
        }
        // Check if it's a valid status code (3 digits)
        if (statusStr.size() >= 3 && statusStr.find_first_not_of("0123456789") == std::string::npos) {
            resp.statusCode = atoi(statusStr.c_str());
            resp.body = output.substr(0, lastNl);
            // Remove trailing \r from body
            if (!resp.body.empty() && resp.body.back() == '\r') resp.body.pop_back();
            return;
        }
    }

    // No valid status code found - treat entire output as body
    resp.statusCode = 0;
    resp.body = output;
}

// Write POST body to temp file, return temp file path
static std::string writeTempBody(const std::string& body) {
    char tempPath[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempPath);
    char tempFile[MAX_PATH] = {0};
    GetTempFileNameA(tempPath, "gpbd", 0, tempFile);
    FILE* f = fopen(tempFile, "wb");
    if (f) {
        fwrite(body.c_str(), 1, body.size(), f);
        fclose(f);
        return std::string(tempFile);
    }
    return "";
}

HttpResponse httpPost(const std::string& url, const std::string& jsonBody, int timeoutSec) {
    HttpResponse resp;

    // Write body to temp file to avoid stdin pipe issues
    std::string tempBody = writeTempBody(jsonBody);
    if (tempBody.empty()) {
        resp.error = "Failed to create temp file for POST body";
        Logger::instance().write("[HTTP] httpPost: writeTempBody failed", LogLevel::Error);
        return resp;
    }

    // Use -d @file to read body from file, -w for status code
    // Note: % not %% — CreateProcessA passes the string as-is, no escaping needed
    std::string args = "-k -s -m " + std::to_string(timeoutSec) +
                       " -X POST -H \"Content-Type: application/json\"" +
                       " -d @" + tempBody +
                       " -w \"\\n%{http_code}\"" +
                       " \"" + url + "\"";

    DWORD exitCode = 0;
    int timeoutMs = timeoutSec * 1000 + 5000;
    std::string output = runCurlExe(args, timeoutMs, &exitCode);

    // Clean up temp file
    DeleteFileA(tempBody.c_str());

    if (output.rfind("ERROR:", 0) == 0) {
        resp.error = output;
        return resp;
    }

    if (exitCode != 0) {
        resp.error = "curl exit code " + std::to_string(exitCode);
        if (!output.empty()) resp.error += ": " + output;
        return resp;
    }

    parseCurlOutput(output, resp);
    return resp;
}

HttpResponse httpGet(const std::string& url, int timeoutSec) {
    HttpResponse resp;

    std::string args = "-k -s -m " + std::to_string(timeoutSec) +
                       " -w \"\\n%{http_code}\"" +
                       " \"" + url + "\"";

    DWORD exitCode = 0;
    int timeoutMs = timeoutSec * 1000 + 5000;
    std::string output = runCurlExe(args, timeoutMs, &exitCode);

    if (output.rfind("ERROR:", 0) == 0) {
        resp.error = output;
        return resp;
    }

    if (exitCode != 0) {
        resp.error = "curl exit code " + std::to_string(exitCode);
        if (!output.empty()) resp.error += ": " + output;
        return resp;
    }

    parseCurlOutput(output, resp);
    return resp;
}

HttpResponse httpDownload(const std::string& url, int timeoutSec) {
    HttpResponse resp;

    // Download to a temp file, then read back (handles binary safely)
    char tempPath[MAX_PATH] = {0};
    GetTempPathA(MAX_PATH, tempPath);
    char tempFile[MAX_PATH] = {0};
    GetTempFileNameA(tempPath, "gpdl", 0, tempFile);

    std::string args = "-k -s -f -m " + std::to_string(timeoutSec) +
                       " -o \"" + std::string(tempFile) + "\"" +
                       " \"" + url + "\"";

    DWORD exitCode = 0;
    int timeoutMs = timeoutSec * 1000 + 5000;
    runCurlExe(args, timeoutMs, &exitCode);

    if (exitCode != 0 && exitCode != 22) { // 22 = HTTP error (e.g. 404)
        resp.error = "curl download exit code " + std::to_string(exitCode);
        DeleteFileA(tempFile);
        return resp;
    }

    // Read the downloaded file
    FILE* f = fopen(tempFile, "rb");
    if (!f) {
        resp.error = "Failed to read downloaded file";
        DeleteFileA(tempFile);
        return resp;
    }

    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fileSize > 0) {
        std::vector<char> data(fileSize);
        fread(data.data(), 1, fileSize, f);
        resp.body.assign(data.begin(), data.end());
        resp.statusCode = 200;
    } else {
        resp.error = "Downloaded file is empty";
    }

    fclose(f);
    DeleteFileA(tempFile);
    return resp;
}

std::string urlEncode(const std::string& s) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

} // namespace gp
