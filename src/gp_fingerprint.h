#pragma once
#include <string>

namespace gp {

// Generate device fingerprint: CPU ProcessorId + Disk Serial + NIC MAC -> SHA256 (32 hex chars)
std::string generateDeviceFingerprint();

} // namespace gp
