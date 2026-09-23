// Durable session state: the epoch counter and the latched kill state must fail closed and must
// never be left half-written.
#include "fastmm/core/session_state.hpp"

#include "test_support.hpp"

#include <filesystem>
#include <fstream>
#include <string>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {
std::string read_all(const std::string& path) {
  std::ifstream in(path);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

TEST_CASE("core.session_state: the epoch counter is durable and fails closed") {
  const auto path = (tmp_dir() / "epoch_durable").string();
  std::filesystem::remove(path);
  const auto a = SessionEpochStore::next_epoch(path);
  REQUIRE(a.has_value());
  CHECK(*a == 1);
  CHECK(read_all(path) == "1\n");
  CHECK(!std::filesystem::exists(path + ".tmp"));  // the temporary file is renamed, not left behind
  const auto b = SessionEpochStore::next_epoch(path);
  REQUIRE(b.has_value());
  CHECK(*b == 2);

  // An unwritable path used to be ignored, so every restart got epoch 1 and reused the previous
  // session's client order ids.
  const auto bad = (tmp_dir() / "no_such_dir" / "epoch").string();
  std::filesystem::remove_all((tmp_dir() / "no_such_dir").string());
  CHECK(!SessionEpochStore::next_epoch(bad).has_value());

  // A file that does not start with a number is an error, not a silent restart from 1.
  const auto junk = (tmp_dir() / "epoch_junk").string();
  {
    std::ofstream out(junk);
    out << "not-a-number\n";
  }
  CHECK(!SessionEpochStore::next_epoch(junk).has_value());
}

TEST_CASE("core.session_state: the epoch cycles through 1..65535 and reports the wrap") {
  const auto path = (tmp_dir() / "epoch_wrap").string();
  const auto set = [&](const char* counter) {
    std::ofstream out(path);
    out << counter << "\n";
  };
  bool wrapped = true;
  set("65534");
  auto e = SessionEpochStore::next_epoch(path, &wrapped);
  REQUIRE(e.has_value());
  CHECK(*e == 65535);
  CHECK(!wrapped);
  // 65536 wraps to 1, not to 0 (which the old code mapped back to 1, issuing it twice in a row).
  e = SessionEpochStore::next_epoch(path, &wrapped);
  REQUIRE(e.has_value());
  CHECK(*e == 1);
  CHECK(wrapped);
  e = SessionEpochStore::next_epoch(path, &wrapped);
  REQUIRE(e.has_value());
  CHECK(*e == 2);
}

TEST_CASE("core.session_state: the kill state round-trips and a corrupt file fails closed") {
  const auto path = (tmp_dir() / "state.kill").string();
  std::filesystem::remove(path);
  const auto empty = KillStateStore::load(path);
  REQUIRE(empty.has_value());
  CHECK(!empty->latched);
  CHECK(empty->carry() == Notional{});

  KillState s;
  s.latched = true;
  s.reason = KillReason::MaxLoss;
  s.realized = Notional::from_decimal("-12.5").value();
  s.fees = Notional::from_decimal("0.75").value();
  s.sessions = 3;
  s.updated_ns = 1234;
  REQUIRE(KillStateStore::store(path, s).has_value());
  const auto back = KillStateStore::load(path);
  REQUIRE(back.has_value());
  CHECK(back->latched);
  CHECK(back->reason == KillReason::MaxLoss);
  CHECK(back->realized == s.realized);
  CHECK(back->fees == s.fees);
  CHECK(back->sessions == 3);
  CHECK(back->carry() == Notional::from_decimal("-13.25").value());

  REQUIRE(KillStateStore::clear(path).has_value());
  CHECK(!std::filesystem::exists(path));
  CHECK(KillStateStore::load(path).has_value());  // a missing file is a cleared state

  {
    std::ofstream out(path);
    out << "latched 1\n";  // no header: not a FastMM kill state file
  }
  CHECK(!KillStateStore::load(path).has_value());
  {
    std::ofstream out(path);
    out << "fastmm-kill 1\nrealized_raw twelve\n";
  }
  CHECK(!KillStateStore::load(path).has_value());
}

TEST_CASE("core.session_state: write_file_atomic replaces the file or leaves the old one") {
  const auto path = (tmp_dir() / "atomic.txt").string();
  REQUIRE(write_file_atomic(path, "first").has_value());
  CHECK(read_all(path) == "first");
  REQUIRE(write_file_atomic(path, "second value").has_value());
  CHECK(read_all(path) == "second value");
  CHECK(!std::filesystem::exists(path + ".tmp"));
  CHECK(!write_file_atomic((tmp_dir() / "absent" / "x").string(), "x").has_value());
}
