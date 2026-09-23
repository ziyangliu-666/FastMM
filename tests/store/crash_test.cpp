// Crash safety: a writer killed mid-batch leaves the committed batches intact and the file
// usable. The store is WAL with synchronous=NORMAL, so a committed batch survives the process
// dying (docs/reference/storage.md).
#include "store_test_util.hpp"

#include "fastmm/store/sqlite_store.hpp"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <memory>
#include <string>

using namespace fastmm;
using namespace fastmm::store;
using fastmm::test::kDay1Ns;
using fastmm::test::tmp_dir;

namespace {

std::string fresh(const char* name) {
  const auto p = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove(p, ec);
  std::filesystem::remove(p.string() + "-wal", ec);
  std::filesystem::remove(p.string() + "-shm", ec);
  return p.string();
}

std::unique_ptr<Backend> open_backend(GenericSection& section, const std::string& path) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.engine_name = "crash";
  auto b = make_sqlite_backend();
  REQUIRE(b->open(o));
  return b;
}

std::unique_ptr<Reader> open_reader(GenericSection& section, const std::string& path) {
  section.values["path"] = path;
  BackendOptions o;
  o.config = &section;
  o.read_only = true;
  auto r = make_sqlite_reader();
  REQUIRE(r->open(o));
  return r;
}

constexpr int kCommitted = 30;  // fills in batches the child commits
constexpr int kPending = 25;    // fills in the batch it is killed inside

}  // namespace

TEST_CASE("store.crash: a writer killed mid-batch loses that batch and nothing else") {
  const std::string path = fresh("crash.db");
  const InstrumentTable table = fastmm::test::store_table();
  // Create the store and the session row in the parent, then close every handle before forking.
  {
    GenericSection section;
    auto backend = open_backend(section, path);
    REQUIRE(backend->session_open(fastmm::test::store_session(77, kDay1Ns)));
    REQUIRE(backend->instruments(77, std::span<const Instrument>(table.data(), table.size())));
    backend->close();
  }

  const pid_t pid = fork();
  REQUIRE(pid >= 0);
  if (pid == 0) {
    // Child: commit kCommitted fills, then open one more batch and die inside it. SIGKILL gives
    // it no chance to roll back, close the file or flush anything.
    GenericSection section;
    auto backend = open_backend(section, path);
    backend->begin();
    for (int i = 0; i < kCommitted; ++i) {
      backend->fill(fastmm::test::store_fill(77,
                                             static_cast<std::uint64_t>(i + 1),
                                             kDay1Ns + i * 1'000'000'000LL,
                                             Side::Buy,
                                             10'000'000'000,
                                             100'000'000,
                                             0,
                                             0,
                                             "C" + std::to_string(i)));
    }
    backend->commit();
    backend->begin();
    for (int i = 0; i < kPending; ++i) {
      backend->fill(fastmm::test::store_fill(77,
                                             static_cast<std::uint64_t>(1000 + i),
                                             kDay1Ns + i * 1'000'000'000LL,
                                             Side::Sell,
                                             10'000'000'000,
                                             100'000'000,
                                             0,
                                             0,
                                             "P" + std::to_string(i)));
    }
    ::raise(SIGKILL);
    _exit(1);  // unreachable
  }

  int status = 0;
  REQUIRE(::waitpid(pid, &status, 0) == pid);
  REQUIRE(WIFSIGNALED(status));
  CHECK(WTERMSIG(status) == SIGKILL);

  SUBCASE("the committed fills are all there and the pending batch is gone") {
    GenericSection section;
    auto reader = open_reader(section, path);
    auto r = reader->fills(QueryFilter{});
    REQUIRE(r);
    CHECK(r->rows.size() == kCommitted);
    for (const auto& row : r->rows) {
      bool sell = false;
      for (const auto& cell : row) sell = sell || cell == "Sell";
      CHECK_FALSE(sell);  // every Sell belonged to the batch that never committed
    }
  }

  SUBCASE("the session it was killed in has no close, which recovery reports") {
    GenericSection section;
    auto reader = open_reader(section, path);
    QueryFilter f;
    f.engine = "test";
    auto r = reader->recovery(f);
    REQUIRE(r);
    REQUIRE(r->found);
    CHECK(r->session_id == 77);
    CHECK_FALSE(r->clean_shutdown);
    CHECK(r->stopped_utc.empty());
  }

  SUBCASE("the store keeps taking writes after the crash") {
    GenericSection section;
    auto backend = open_backend(section, path);
    backend->begin();
    backend->fill(fastmm::test::store_fill(
        77, 5000, kDay1Ns, Side::Buy, 10'000'000'000, 100'000'000, 0, 0, "AFTER"));
    backend->commit();
    CHECK(backend->errors() == 0);
    SessionClose c;
    c.session_id = 77;
    c.stopped_ns = kDay1Ns + 60'000'000'000LL;
    REQUIRE(backend->session_close(c));
    backend->close();

    GenericSection read_section;
    auto reader = open_reader(read_section, path);
    auto r = reader->fills(QueryFilter{});
    REQUIRE(r);
    CHECK(r->rows.size() == kCommitted + 1);
  }
}
