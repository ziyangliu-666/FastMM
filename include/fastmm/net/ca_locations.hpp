#pragma once
// Where a TLS client context (TlsContext, src/net/tls_stream.cpp) finds CA certificates. The first
// step that yields something wins:
//
//   1. SSL_CERT_FILE and SSL_CERT_DIR, when either is set and non-empty (both when both are set)
//   2. the first regular file among kSystemCaFiles
//   3. the fallback file, when set and a regular file (fastmm_live sets certifi.where())
//   4. OpenSSL's compiled-in defaults (SSL_CTX_set_default_verify_paths)
//
// Step 4 keeps system-OpenSSL builds working on distributions with other bundle paths; a static
// OpenSSL in a wheel has no usable compiled-in path, hence steps 2 and 3.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::net {

enum class CaSource : std::uint8_t { Environment, SystemFile, Fallback, OpenSslDefault };

struct CaLocations {
  CaSource source = CaSource::OpenSslDefault;
  std::string file;  // empty: none
  std::string dir;   // empty: none (only from SSL_CERT_DIR)
};

inline constexpr std::array<std::string_view, 3> kSystemCaFiles = {
    "/etc/ssl/certs/ca-certificates.crt",  // Debian, Ubuntu, Arch, Gentoo
    "/etc/pki/tls/certs/ca-bundle.crt",    // Fedora, RHEL, CentOS, Alma, Rocky
    "/etc/ssl/cert.pem",                   // Alpine, macOS, some BSDs
};

// Process-wide step 3. Thread-safe; set it before creating TLS contexts.
void set_ca_fallback_file(std::string path);
std::string ca_fallback_file();

// Resolves the steps above against the current environment and filesystem.
CaLocations find_ca_locations(std::span<const std::string_view> system_files,
                              const std::string& fallback_file);
// find_ca_locations(kSystemCaFiles, ca_fallback_file())
CaLocations find_ca_locations();

std::string_view ca_source_name(CaSource s) noexcept;

}  // namespace fastmm::net
