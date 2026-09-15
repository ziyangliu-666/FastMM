#include "fastmm/net/ca_locations.hpp"

#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <utility>

namespace fastmm::net {

namespace {

std::mutex g_fallback_mu;
std::string g_fallback;  // guarded by g_fallback_mu

std::string env_or_empty(const char* name) {
  // Read once per TLS context creation, never on the hot path.
  const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
  return v != nullptr ? std::string(v) : std::string();
}

bool is_regular_file(const std::string& path) {
  std::error_code ec;
  return !path.empty() && std::filesystem::is_regular_file(path, ec);
}

}  // namespace

void set_ca_fallback_file(std::string path) {
  const std::lock_guard<std::mutex> lock(g_fallback_mu);
  g_fallback = std::move(path);
}

std::string ca_fallback_file() {
  const std::lock_guard<std::mutex> lock(g_fallback_mu);
  return g_fallback;
}

CaLocations find_ca_locations(std::span<const std::string_view> system_files,
                              const std::string& fallback_file) {
  CaLocations out;
  out.file = env_or_empty("SSL_CERT_FILE");
  out.dir = env_or_empty("SSL_CERT_DIR");
  if (!out.file.empty() || !out.dir.empty()) {
    out.source = CaSource::Environment;
    return out;
  }
  for (const std::string_view f : system_files) {
    std::string path(f);
    if (is_regular_file(path)) {
      out.source = CaSource::SystemFile;
      out.file = std::move(path);
      return out;
    }
  }
  if (is_regular_file(fallback_file)) {
    out.source = CaSource::Fallback;
    out.file = fallback_file;
    return out;
  }
  out.source = CaSource::OpenSslDefault;
  return out;
}

CaLocations find_ca_locations() {
  return find_ca_locations(kSystemCaFiles, ca_fallback_file());
}

std::string_view ca_source_name(CaSource s) noexcept {
  switch (s) {
    case CaSource::Environment:
      return "environment";
    case CaSource::SystemFile:
      return "system";
    case CaSource::Fallback:
      return "fallback";
    case CaSource::OpenSslDefault:
      return "openssl-default";
  }
  return "unknown";
}

}  // namespace fastmm::net
