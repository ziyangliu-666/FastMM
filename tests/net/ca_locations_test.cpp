#include "fastmm/net/ca_locations.hpp"

#include "net_test_util.hpp"

#include "fastmm/net/tls_stream.hpp"

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

using namespace fastmm::net;
using namespace fastmm::net::test;

namespace {

// Sets or unsets an environment variable for one scope and restores the previous value.
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    if (const char* old = std::getenv(name))  // NOLINT(concurrency-mt-unsafe)
      old_ = std::string(old);
    if (value != nullptr) {
      ::setenv(name, value, 1);  // NOLINT(concurrency-mt-unsafe)
    } else {
      ::unsetenv(name);  // NOLINT(concurrency-mt-unsafe)
    }
  }
  ~ScopedEnv() {
    if (old_) {
      ::setenv(name_, old_->c_str(), 1);  // NOLINT(concurrency-mt-unsafe)
    } else {
      ::unsetenv(name_);  // NOLINT(concurrency-mt-unsafe)
    }
  }
  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  const char* name_;
  std::optional<std::string> old_;
};

// A scratch directory with named files; find_ca_locations only checks that they exist. One per
// process: ctest runs the test cases of this file as parallel processes.
struct CaDir {
  std::filesystem::path root;
  CaDir() : root(fastmm::test::tmp_dir() / ("ca_locations_" + std::to_string(::getpid()))) {
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "hashed");
  }
  ~CaDir() {
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
  }
  CaDir(const CaDir&) = delete;
  CaDir& operator=(const CaDir&) = delete;

  std::string file(const char* name) const {
    const std::filesystem::path p = root / name;
    std::ofstream(p) << "placeholder\n";
    return p.string();
  }
  std::string missing(const char* name) const { return (root / name).string(); }
  std::string dir() const { return (root / "hashed").string(); }
};

}  // namespace

TEST_CASE("ca_locations: SSL_CERT_FILE and SSL_CERT_DIR come before system files and fallback") {
  const CaDir d;
  const std::string sys = d.file("system.pem");
  const std::string fallback = d.file("certifi.pem");
  const std::string env_file = d.file("env.pem");
  const std::string env_dir = d.dir();
  const std::vector<std::string_view> system_files{sys};

  SUBCASE("both variables") {
    const ScopedEnv f("SSL_CERT_FILE", env_file.c_str());
    const ScopedEnv dd("SSL_CERT_DIR", env_dir.c_str());
    const CaLocations ca = find_ca_locations(system_files, fallback);
    CHECK(ca.source == CaSource::Environment);
    CHECK(ca.file == env_file);
    CHECK(ca.dir == env_dir);
  }
  SUBCASE("SSL_CERT_FILE alone") {
    const ScopedEnv f("SSL_CERT_FILE", env_file.c_str());
    const ScopedEnv dd("SSL_CERT_DIR", nullptr);
    const CaLocations ca = find_ca_locations(system_files, fallback);
    CHECK(ca.source == CaSource::Environment);
    CHECK(ca.file == env_file);
    CHECK(ca.dir.empty());
  }
  SUBCASE("SSL_CERT_DIR alone") {
    const ScopedEnv f("SSL_CERT_FILE", nullptr);
    const ScopedEnv dd("SSL_CERT_DIR", env_dir.c_str());
    const CaLocations ca = find_ca_locations(system_files, fallback);
    CHECK(ca.source == CaSource::Environment);
    CHECK(ca.file.empty());
    CHECK(ca.dir == env_dir);
  }
  SUBCASE("empty values count as unset") {
    const ScopedEnv f("SSL_CERT_FILE", "");
    const ScopedEnv dd("SSL_CERT_DIR", "");
    const CaLocations ca = find_ca_locations(system_files, fallback);
    CHECK(ca.source == CaSource::SystemFile);
    CHECK(ca.file == sys);
  }
}

TEST_CASE("ca_locations: first existing system file, then the fallback, then OpenSSL defaults") {
  const ScopedEnv f("SSL_CERT_FILE", nullptr);
  const ScopedEnv dd("SSL_CERT_DIR", nullptr);
  const CaDir d;
  const std::string second = d.file("second.pem");
  const std::string third = d.file("third.pem");
  const std::string fallback = d.file("certifi.pem");
  const std::string absent = d.missing("absent.pem");
  const std::string directory = d.dir();

  SUBCASE("system files in list order, skipping missing paths and directories") {
    const std::vector<std::string_view> files{absent, directory, second, third};
    const CaLocations ca = find_ca_locations(files, fallback);
    CHECK(ca.source == CaSource::SystemFile);
    CHECK(ca.file == second);
    CHECK(ca.dir.empty());
  }
  SUBCASE("fallback when no system file exists") {
    const std::vector<std::string_view> files{absent, directory};
    const CaLocations ca = find_ca_locations(files, fallback);
    CHECK(ca.source == CaSource::Fallback);
    CHECK(ca.file == fallback);
  }
  SUBCASE("OpenSSL defaults when the fallback is unset or missing") {
    const std::vector<std::string_view> files{absent};
    CHECK(find_ca_locations(files, "").source == CaSource::OpenSslDefault);
    const CaLocations ca = find_ca_locations(files, d.missing("gone.pem"));
    CHECK(ca.source == CaSource::OpenSslDefault);
    CHECK(ca.file.empty());
    CHECK(ca.dir.empty());
  }
}

TEST_CASE("ca_locations: default system list and the process-wide fallback") {
  CHECK(kSystemCaFiles[0] == "/etc/ssl/certs/ca-certificates.crt");
  CHECK(kSystemCaFiles[1] == "/etc/pki/tls/certs/ca-bundle.crt");
  CHECK(kSystemCaFiles[2] == "/etc/ssl/cert.pem");
  const std::string before = ca_fallback_file();
  set_ca_fallback_file("/some/certifi/cacert.pem");
  CHECK(ca_fallback_file() == "/some/certifi/cacert.pem");
  set_ca_fallback_file(before);
  CHECK(ca_source_name(CaSource::Fallback) == "fallback");
}

TEST_CASE("ca_locations: a client context loads SSL_CERT_FILE and names it when it cannot") {
  const ScopedEnv dd("SSL_CERT_DIR", nullptr);
  const std::string cert = tls_fixture("cert.pem").string();
  SUBCASE("valid bundle") {
    const ScopedEnv f("SSL_CERT_FILE", cert.c_str());
    CHECK_NOTHROW(TlsContext{});
  }
  SUBCASE("missing file") {
    const CaDir d;
    const std::string absent = d.missing("absent.pem");
    const ScopedEnv f("SSL_CERT_FILE", absent.c_str());
    std::string what;
    try {
      const TlsContext ctx;
    } catch (const std::runtime_error& e) {
      what = e.what();
    }
    CHECK(what.find("SSL_CERT_FILE=" + absent) != std::string::npos);
  }
  SUBCASE("SSL_CERT_DIR that is not a directory") {
    const ScopedEnv f("SSL_CERT_FILE", nullptr);
    const ScopedEnv d2("SSL_CERT_DIR", cert.c_str());
    CHECK_THROWS_WITH_AS(TlsContext{}, doctest::Contains("SSL_CERT_DIR="), std::runtime_error);
  }
}
