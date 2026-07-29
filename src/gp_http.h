#pragma once
#include <string>

namespace gp {

// HTTP wrapper using curl.exe (handles corporate TLS middlebox)
struct HttpResponse {
    int statusCode = 0;
    std::string body;
    std::string error;
    bool ok() const { return statusCode >= 200 && statusCode < 300; }
};

// HTTPS POST with JSON body
HttpResponse httpPost(const std::string& url, const std::string& jsonBody, int timeoutSec = 10);

// HTTPS GET
HttpResponse httpGet(const std::string& url, int timeoutSec = 15);

// HTTPS GET binary (returns raw bytes)
HttpResponse httpDownload(const std::string& url, int timeoutSec = 300);

// URL-encode a string
std::string urlEncode(const std::string& s);

} // namespace gp
