#include "gp_fingerprint.h"
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>
#include <bcrypt.h>
#include <sstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <iomanip>

namespace gp {

// Run a command and capture stdout
static std::string runCommand(const std::string& cmd) {
    std::string result;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = {sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};

    std::string fullCmd = "cmd /c " + cmd;
    if (CreateProcessA(nullptr, const_cast<char*>(fullCmd.c_str()), nullptr, nullptr, TRUE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(hWrite);
        char buf[4096];
        DWORD read = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &read, nullptr) && read > 0) {
            result.append(buf, read);
        }
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(hRead);
    CloseHandle(hWrite);
    return result;
}

// Extract value from "Key=Value\r\n" format (wmic output)
static std::string extractValue(const std::string& output, const std::string& key) {
    // wmic /value output looks like:\r\nKey=Value\r\n
    std::string search = key + "=";
    size_t pos = output.find(search);
    if (pos == std::string::npos) return "";
    pos += search.size();
    size_t end = pos;
    while (end < output.size() && output[end] != '\r' && output[end] != '\n') end++;
    std::string val = output.substr(pos, end - pos);
    // Trim whitespace
    size_t start = val.find_first_not_of(" \t");
    if (start == std::string::npos) return "";
    size_t stop = val.find_last_not_of(" \t\r\n");
    return val.substr(start, stop - start + 1);
}

static bool isVirtualAdapter(const std::string& desc) {
    std::string lower = desc;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    const char* keywords[] = {"tap", "vpn", "wireguard", "tunnel", "virtual", "hyper-v", "ericvpn"};
    for (auto kw : keywords) {
        if (lower.find(kw) != std::string::npos) return true;
    }
    return false;
}

static std::string getCpuProcessorId() {
    std::string out = runCommand("wmic cpu get ProcessorId /value");
    return extractValue(out, "ProcessorId");
}

static std::string getDiskSerial() {
    std::string out = runCommand("wmic diskdrive where Index=0 get SerialNumber /value");
    return extractValue(out, "SerialNumber");
}

static std::string getMainNicMac() {
    ULONG bufLen = 15000;
    std::vector<BYTE> buf(bufLen);

    ULONG ret = 0;
    DWORD flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    DWORD dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                       reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    if (dwRet == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        dwRet = GetAdaptersAddresses(AF_INET, flags, nullptr,
                                     reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data()), &bufLen);
    }
    if (dwRet != NO_ERROR) return "";

    auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
    for (; pAddr; pAddr = pAddr->Next) {
        if (pAddr->OperStatus != IfOperStatusUp) continue;
        if (pAddr->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        // Convert description to narrow string for keyword check
        char desc[256] = {0};
        WideCharToMultiByte(CP_UTF8, 0, pAddr->Description, -1, desc, sizeof(desc), nullptr, nullptr);
        if (isVirtualAdapter(desc)) continue;

        // Format MAC
        std::ostringstream mac;
        for (ULONG i = 0; i < pAddr->PhysicalAddressLength; i++) {
            mac << std::uppercase << std::setfill('0') << std::setw(2) << std::hex
                << (int)pAddr->PhysicalAddress[i];
        }
        return mac.str();
    }
    return "";
}

static std::string sha256Hex32(const std::string& input) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<UCHAR> hash(32);

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return "";
    }
    BCryptHashData(hHash, const_cast<UCHAR*>(reinterpret_cast<const UCHAR*>(input.data())),
                   (ULONG)input.size(), 0);
    BCryptFinishHash(hHash, hash.data(), 32, 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    std::ostringstream hex;
    for (int i = 0; i < 16; i++) { // only first 16 bytes = 32 hex chars
        hex << std::nouppercase << std::setfill('0') << std::setw(2) << std::hex << (int)hash[i];
    }
    return hex.str();
}

std::string generateDeviceFingerprint() {
    std::vector<std::string> parts;

    std::string cpuId = getCpuProcessorId();
    if (!cpuId.empty()) parts.push_back(cpuId);

    std::string diskSn = getDiskSerial();
    if (!diskSn.empty()) parts.push_back(diskSn);

    std::string mac = getMainNicMac();
    if (!mac.empty()) parts.push_back(mac);

    if (parts.empty()) return "unknown";

    std::string raw;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i > 0) raw += "|";
        raw += parts[i];
    }

    return sha256Hex32(raw);
}

} // namespace gp
