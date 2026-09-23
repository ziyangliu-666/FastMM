#include "fastmm/core/session_state.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace fastmm {

namespace {

std::string why(const std::string& what, const std::string& path) {
  return what + " " + path + ": " + std::strerror(errno);
}

// fsync the directory holding `path` so the rename itself is durable.
void sync_parent(const std::string& path) noexcept {
  const std::filesystem::path p(path);
  const std::string dir = p.has_parent_path() ? p.parent_path().string() : std::string(".");
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return;
  static_cast<void>(::fsync(fd));
  static_cast<void>(::close(fd));
}

constexpr std::uint64_t kEpochPeriod = 65535;  // epochs cycle through 1..65535

}  // namespace

Result<void, std::string> write_file_atomic(const std::string& path, std::string_view contents) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return fail(why("cannot create", tmp));
  const char* p = contents.data();
  std::size_t left = contents.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n <= 0) {
      if (errno == EINTR) continue;
      const std::string e = why("cannot write", tmp);
      static_cast<void>(::close(fd));
      static_cast<void>(::unlink(tmp.c_str()));
      return fail(e);
    }
    p += n;
    left -= static_cast<std::size_t>(n);
  }
  if (::fsync(fd) != 0) {
    const std::string e = why("cannot fsync", tmp);
    static_cast<void>(::close(fd));
    static_cast<void>(::unlink(tmp.c_str()));
    return fail(e);
  }
  if (::close(fd) != 0) {
    const std::string e = why("cannot close", tmp);
    static_cast<void>(::unlink(tmp.c_str()));
    return fail(e);
  }
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    const std::string e = why("cannot rename onto", path);
    static_cast<void>(::unlink(tmp.c_str()));
    return fail(e);
  }
  sync_parent(path);
  return {};
}

// ---- session epoch ----------------------------------------------------------------------------

Result<std::uint16_t, std::string> SessionEpochStore::next_epoch(const std::string& path,
                                                                bool* wrapped) {
  if (wrapped != nullptr) *wrapped = false;
  std::uint64_t counter = 0;
  std::error_code ec;
  if (std::filesystem::exists(path, ec)) {
    std::ifstream in(path);
    if (!in) return fail(why("cannot read session epoch file", path));
    std::string first;
    std::getline(in, first);
    std::istringstream is(first);
    if (!(is >> counter))
      return fail("session epoch file " + path + " does not start with a number");
  } else if (ec) {
    return fail("cannot stat session epoch file " + path + ": " + ec.message());
  }
  const std::uint64_t next = counter + 1;
  if (auto r = write_file_atomic(path, std::to_string(next) + "\n"); !r) return fail(r.error());
  if (wrapped != nullptr && next > kEpochPeriod) *wrapped = true;
  return static_cast<std::uint16_t>(1 + (next - 1) % kEpochPeriod);
}

// ---- latched risk state -----------------------------------------------------------------------

std::string KillStateStore::default_path(std::string_view journal_dir,
                                         std::string_view engine_name) {
  std::string dir(journal_dir.empty() ? std::string_view(".") : journal_dir);
  return dir + "/" + std::string(engine_name) + ".kill";
}

Result<KillState, std::string> KillStateStore::load(const std::string& path) {
  KillState s;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    if (ec) return fail("cannot stat kill state file " + path + ": " + ec.message());
    return s;
  }
  std::ifstream in(path);
  if (!in) return fail(why("cannot read kill state file", path));
  std::string key;
  std::string value;
  bool header = false;
  try {
    while (in >> key >> value) {
      if (key == "fastmm-kill") {
        header = value == "1";
      } else if (key == "latched") {
        s.latched = value != "0";
      } else if (key == "reason") {
        s.reason = static_cast<KillReason>(std::stoul(value) & 0xFFU);
      } else if (key == "realized_raw") {
        s.realized = Notional::from_raw(std::stoll(value));
      } else if (key == "fees_raw") {
        s.fees = Notional::from_raw(std::stoll(value));
      } else if (key == "sessions") {
        s.sessions = std::stoull(value);
      } else if (key == "updated_ns") {
        s.updated_ns = std::stoll(value);
      } else {
        return fail("kill state file " + path + ": unknown key '" + key + "'");
      }
    }
  } catch (const std::exception& e) {
    return fail("kill state file " + path + ": bad value for '" + key + "': " + e.what());
  }
  if (!header) return fail("kill state file " + path + " is not a FastMM kill state file");
  return s;
}

Result<void, std::string> KillStateStore::store(const std::string& path, const KillState& s) {
  std::string t = "fastmm-kill 1\n";
  t += "latched " + std::string(s.latched ? "1" : "0") + "\n";
  t += "reason " + std::to_string(static_cast<unsigned>(s.reason)) + "\n";
  t += "realized_raw " + std::to_string(s.realized.raw) + "\n";
  t += "fees_raw " + std::to_string(s.fees.raw) + "\n";
  t += "sessions " + std::to_string(s.sessions) + "\n";
  t += "updated_ns " + std::to_string(s.updated_ns) + "\n";
  return write_file_atomic(path, t);
}

Result<void, std::string> KillStateStore::clear(const std::string& path) {
  std::error_code ec;
  std::filesystem::remove(path, ec);
  if (ec) return fail("cannot remove kill state file " + path + ": " + ec.message());
  return {};
}

}  // namespace fastmm
