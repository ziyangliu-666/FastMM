// The journal archive (core/journal_archive.hpp): compression of the journals nobody writes, their
// reading back, retention by age and the cap on the directory, and the parts in use left alone.
#include "fastmm/core/journal_archive.hpp"

#include "test_support.hpp"

#include "fastmm/core/journal.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace fastmm;
using fastmm::test::tmp_dir;

namespace {

InstrumentTable make_table() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.tick = Price::from_decimal("0.01").value();
  i.lot = Qty::from_decimal("0.001").value();
  i.flags = Instrument::kEnabled;
  REQUIRE(t.add(i));
  return t;
}

JournalSessionInfo make_info(const InstrumentTable& t) {
  JournalSessionInfo info;
  info.session_id = 11;
  info.start_ts = Timestamp{1'700'000'000'000'000'000LL};
  info.strategy = "basic_mm";
  info.instruments = &t;
  return info;
}

std::filesystem::path fresh_dir(const char* name) {
  const auto d = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove_all(d, ec);
  std::filesystem::create_directories(d);
  return d;
}

// A complete journal of `n` trades at `path`.
void write_journal(const std::string& path, const InstrumentTable& table, int n) {
  MsgRing ring(1 << 22);
  JournalFileWriter fw(ring, path, make_info(table));
  REQUIRE(fw.ok());
  JournalWriter w(&ring);
  for (int i = 0; i < n; ++i) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
    t.trade_id = static_cast<std::uint64_t>(i);
    t.price = Price::from_int(100 + (i % 7));
    REQUIRE(w.record(t.hdr));
    if (i % 4096 == 4095) static_cast<void>(fw.drain_once());
  }
  static_cast<void>(fw.drain_once());
  fw.stop();
  REQUIRE_FALSE(fw.failed());
}

void age(const std::filesystem::path& p, std::chrono::hours by) {
  std::filesystem::last_write_time(p, std::filesystem::file_time_type::clock::now() - by);
}

std::vector<std::uint64_t> trade_ids(JournalReader& r) {
  std::vector<std::uint64_t> ids;
  r.for_each([&](const EventHeader* h) {
    if (h->type == EventType::Trade) ids.push_back(msg_cast<TradeMsg>(h).trade_id);
  });
  return ids;
}

JournalRetention compress_only() {
  JournalRetention r;
  r.compress = true;
  r.idle_s = 0;
  return r;
}

}  // namespace

TEST_CASE("core.journal_archive: a journal nobody writes is compressed and reads back the same") {
  const auto dir = fresh_dir("archive_compress");
  const InstrumentTable table = make_table();
  const std::string path = (dir / "s.fmj").string();
  write_journal(path, table, 20'000);
  JournalReader before;
  REQUIRE(before.open(path));
  const std::vector<std::uint64_t> ids = trade_ids(before);
  const auto written = std::filesystem::last_write_time(path);
  const auto bytes = std::filesystem::file_size(path);

  const JournalSweep s = sweep_journals(dir.string(), compress_only());
  CHECK(s.error.empty());
  CHECK(s.compressed == 1);
  CHECK(s.compressed_from == bytes);
  CHECK(s.compressed_to < bytes / 4);
  CHECK_FALSE(std::filesystem::exists(path));
  REQUIRE(std::filesystem::exists(path + ".zst"));
  // The archive keeps the journal's time: retention and the cap go by when it was written.
  CHECK(std::filesystem::last_write_time(path + ".zst") == written);
  CHECK(journal_exists(path));
  CHECK(journal_base_path(path + ".zst") == path);

  // Asked for by either name, the reader gives the same journal.
  for (const std::string& name : {path, path + ".zst"}) {
    JournalReader r;
    REQUIRE_MESSAGE(r.open(name), "cannot open " << name);
    CHECK(r.complete());
    CHECK(r.header().session_id == 11);
    REQUIRE(r.instruments().size() == 1);
    CHECK(r.instruments()[0].symbol == "BTCUSDT");
    CHECK(r.message_count() == before.message_count());
    CHECK(trade_ids(r) == ids);
  }
  // A second sweep has nothing left to do.
  const JournalSweep again = sweep_journals(dir.string(), compress_only());
  CHECK(again.compressed == 0);
  CHECK(again.total_bytes == s.compressed_to);
}

TEST_CASE("core.journal_archive: a part being written or written just now is left alone") {
  const auto dir = fresh_dir("archive_in_use");
  const InstrumentTable table = make_table();
  const std::string done = (dir / "done.fmj").string();
  write_journal(done, table, 1'000);
  const std::string live = (dir / "live.fmj").string();
  MsgRing ring(1 << 20);
  JournalFileWriter fw(ring, live, make_info(table));
  REQUIRE(fw.ok());
  age(live, std::chrono::hours(24 * 30));  // old as it may look, its writer holds it

  JournalRetention r = compress_only();
  r.retention_days = 7;
  r.max_total_bytes = 1;
  const JournalSweep s = sweep_journals(dir.string(), r);
  CHECK(s.compressed == 1);
  CHECK(std::filesystem::exists(live));
  CHECK_FALSE(std::filesystem::exists(live + ".zst"));
  // The cap took the closed journal and could not take the one being written.
  CHECK_FALSE(std::filesystem::exists(done + ".zst"));
  CHECK(s.removed_cap == 1);
  CHECK(s.over_cap);
  fw.stop();

  // Written to within idle_s: in use for an archiver that cannot see a lock (an older writer).
  JournalRetention idle = compress_only();
  idle.idle_s = 3600;
  const JournalSweep t = sweep_journals(dir.string(), idle);
  CHECK(t.compressed == 0);
  CHECK(std::filesystem::exists(live));
}

TEST_CASE("core.journal_archive: retention by age and the cap on the directory, oldest first") {
  const auto dir = fresh_dir("archive_retention");
  const InstrumentTable table = make_table();
  const std::string a = (dir / "a.fmj").string();
  const std::string b = (dir / "b.fmj").string();
  const std::string c = (dir / "c.fmj").string();
  write_journal(a, table, 2'000);
  write_journal(b, table, 2'000);
  write_journal(c, table, 2'000);
  {
    std::ofstream other(dir / "notes.txt");
    other << "not a journal";
  }
  age(a, std::chrono::hours(24 * 10));
  age(b, std::chrono::hours(24 * 3));
  age(c, std::chrono::hours(1));
  // A compression the process died in leaves its temporary file; the sweep removes it.
  {
    std::ofstream tmp(dir / "x.fmj.zst.tmp");
    tmp << "partial";
  }
  age(dir / "x.fmj.zst.tmp", std::chrono::hours(1));

  JournalRetention r;
  r.retention_days = 7;
  r.idle_s = 0;
  JournalSweep s = sweep_journals(dir.string(), r);
  CHECK(s.removed_old == 1);
  CHECK_FALSE(std::filesystem::exists(a));
  CHECK(std::filesystem::exists(b));
  CHECK_FALSE(std::filesystem::exists(dir / "x.fmj.zst.tmp"));
  CHECK(std::filesystem::exists(dir / "notes.txt"));

  // Compressed, then capped at what the newest archive alone takes.
  r.compress = true;
  s = sweep_journals(dir.string(), r);
  CHECK(s.compressed == 2);
  const auto newest = std::filesystem::file_size(c + ".zst");
  r.max_total_bytes = newest;
  s = sweep_journals(dir.string(), r);
  CHECK(s.removed_cap == 1);
  CHECK_FALSE(s.over_cap);
  CHECK(s.total_bytes == newest);
  CHECK_FALSE(std::filesystem::exists(b + ".zst"));
  CHECK(std::filesystem::exists(c + ".zst"));
  // prune_journals counts archives too.
  age(c + ".zst", std::chrono::hours(24 * 30));
  CHECK(prune_journals(dir.string(), 7) == 1);
  CHECK_FALSE(std::filesystem::exists(c + ".zst"));
}

TEST_CASE("core.journal_archive: a compression asked to stop leaves no archive behind") {
  const auto dir = fresh_dir("archive_stop");
  const InstrumentTable table = make_table();
  const std::string path = (dir / "s.fmj").string();
  write_journal(path, table, 5'000);
  const std::atomic<bool> stop{true};
  std::string error;
  CHECK_FALSE(compress_journal(path, path + ".zst", 3, &stop, &error));
  CHECK(error == "stopped");
  CHECK(std::filesystem::exists(path));
  CHECK_FALSE(std::filesystem::exists(path + ".zst"));
  CHECK_FALSE(std::filesystem::exists(path + ".zst.tmp"));
}

TEST_CASE("core.journal_archive: the archiver thread sweeps at once and stops when asked") {
  const auto dir = fresh_dir("archive_thread");
  const InstrumentTable table = make_table();
  const std::string path = (dir / "s.fmj").string();
  write_journal(path, table, 1'000);
  JournalArchiver archiver(dir.string(), compress_only(), seconds(3600));
  archiver.start();
  for (int i = 0; i < 500 && archiver.sweeps() == 0; ++i)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  archiver.stop();
  CHECK(archiver.sweeps() >= 1);
  CHECK(archiver.compressed() == 1);
  CHECK(std::filesystem::exists(path + ".zst"));
}
