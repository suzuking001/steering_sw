#pragma once
#include <string>

namespace sw {
// All local-network clients may configure the device. Keep automatic browser
// protections (canonical host and CSRF) without a user account or login step.
inline bool canonicalLocalHost(const std::string& host, const std::string& localAddress) {
  if (localAddress.empty() || localAddress == "0.0.0.0") return false;
  return host == localAddress || host == localAddress + ":80" ||
         host == "steering-sw.local" || host == "steering-sw.local:80";
}
inline bool checkAccess(bool canonicalHost, bool write, bool csrfMatches) {
  return canonicalHost && (!write || csrfMatches);
}
}
