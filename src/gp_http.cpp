#include "gp_http.h"
#include <windows.h>
#include <winhttp.h>
#include <vector>
#include <cstring>

namespace gp {

// Parse URL into host, path, port, isHttps
struct UrlParts {
    std::wstring host;
    std::wstring path;
    int port = 443;
    bool isHttps = true;
};

static bool parseUrl(const std::string& url, UrlParts& out) {
    // Expect: https://host:port/path or http://host:port/path
    const char* p = url.c_str();
    bool https = true;
    if (strncmp(p, "https://", 8) == 0) { https = true; p += 8; }
    else if (strncmp(p, "http://", 7) == 0) { https = false; p += 7; }
    else return false;

    out.isHttps = https;
    out.port = https ? 443 : 80;

    // Find end of host:port (next / or end)
    const char* pathStart = strchr(p, '/');
    std::string hostPort;
    if (pathStart) {
        hostPort.assign(p, pathStart - p);
        out.path = std::wstring(pathStart, pathStart + strlen(pathStart));
        // Convert to wide
        int len = MultiByteToWideChar(CP_UTF8, 0, pathStart, -1, nullptr, 0);
        std::wstring ws(len, 0);
        MultiByteToWideChar(CP_UTF8, 0, pathStart, -1, &ws[0], len);
        out.path = ws;
        if (!out.path.empty() && out.path.back() == 0) out.path.pop_back();
    } else {
        hostPort.assign(p);
        out.path = L"/";
    }

    // Split host:port
    auto colon = hostPort.find(':');
    if (colon != std::string::npos) {
        std::string host = hostPort.substr(0, colon);
        std::string portStr = hostPort.substr(colon + 1);
        out.port = atoi(portStr.c_str());
        int len = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
        out.host.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, &out.host[0], len);
        if (!out.host.empty() && out.host.back() == 0) out.host.pop_back();
    } else {
        int len = MultiByteToWideChar(CP_UTF8, 0, hostPort.c_str(), -1, nullptr, 0);
        out.host.resize(len);
        MultiByteToWideChar(CP_UTF8, 0, hostPort.c_str(), -1, &out.host[0], len);
        if (!out.host.empty() && out.host.back() == 0) out.host.pop_back();
    }

    return true;
}

static HttpResponse doRequest(const std::string& url, const std::string& method,
                              const std::string& body, int timeoutSec) {
    HttpResponse resp;

    UrlParts parts;
    if (!parseUrl(url, parts)) {
        resp.error = "Invalid URL";
        return resp;
    }

    HINTERNET hSession = WinHttpOpen(L"GatewayPolicy",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { resp.error = "WinHttpOpen failed"; return resp; }

    WinHttpSetTimeouts(hSession, timeoutSec * 1000, timeoutSec * 1000,
                       timeoutSec * 1000, timeoutSec * 1000);

    HINTERNET hConnect = WinHttpConnect(hSession, parts.host.c_str(),
        parts.port, 0);
    if (!hConnect) { resp.error = "WinHttpConnect failed"; WinHttpCloseHandle(hSession); return resp; }

    DWORD flags = WINHTTP_FLAG_REFRESH;
    if (parts.isHttps) flags |= WINHTTP_FLAG_SECURE;

    HINTERNET hRequest = WinHttpOpenRequest(hConnect,
        (method == "POST") ? L"POST" : L"GET",
        parts.path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) { resp.error = "WinHttpOpenRequest failed"; WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return resp; }

    // Ignore self-signed cert errors
    if (parts.isHttps) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                        &secFlags, sizeof(secFlags));
    }

    BOOL bResult = FALSE;
    if (method == "POST") {
        const wchar_t* headers = L"Content-Type: application/json\r\n";
        bResult = WinHttpSendRequest(hRequest, headers, (DWORD)-1,
            (LPVOID)body.c_str(), (DWORD)body.size(), (DWORD)body.size(), 0);
    } else {
        bResult = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    }

    if (!bResult) {
        resp.error = "WinHttpSendRequest failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return resp;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        resp.error = "WinHttpReceiveResponse failed: " + std::to_string(GetLastError());
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession);
        return resp;
    }

    // Status code
    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                       WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    resp.statusCode = (int)statusCode;

    // Read body
    DWORD bytesAvailable = 0;
    DWORD bytesRead = 0;
    std::vector<char> bodyBuf;
    while (WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> chunk(bytesAvailable);
        if (WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead) && bytesRead > 0) {
            bodyBuf.insert(bodyBuf.end(), chunk.begin(), chunk.begin() + bytesRead);
        } else break;
    }
    resp.body.assign(bodyBuf.begin(), bodyBuf.end());

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return resp;
}

HttpResponse httpPost(const std::string& url, const std::string& jsonBody, int timeoutSec) {
    return doRequest(url, "POST", jsonBody, timeoutSec);
}

HttpResponse httpGet(const std::string& url, int timeoutSec) {
    return doRequest(url, "GET", "", timeoutSec);
}

HttpResponse httpDownload(const std::string& url, int timeoutSec) {
    return doRequest(url, "GET", "", timeoutSec);
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
