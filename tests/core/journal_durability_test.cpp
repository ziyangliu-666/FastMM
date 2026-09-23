// Journal lifecycle: sync modes, size rotation, retention and the verdict a replay reads
// (core/journal.hpp, docs/reference/journal-format.md).
#include "test_support.hpp"

#include "fastmm/core/journal.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
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
  info.session_id = 7;
  info.start_ts = Timestamp{1'700'000'000'000'000'000LL};
  info.strategy = "basic_mm";
  info.instruments = &t;
  return info;
}

void write_trades(JournalWriter& w, int n) {
  for (int i = 0; i < n; ++i) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
    t.trade_id = static_cast<std::uint64_t>(i);
    REQUIRE(w.record(t.hdr));
  }
}

std::filesystem::path fresh_dir(const char* name) {
  const auto d = tmp_dir() / name;
  std::error_code ec;
  std::filesystem::remove_all(d, ec);
  std::filesystem::create_directories(d);
  return d;
}

}  // namespace

TEST_CASE("core.journal: fdatasync is a valid sync mode and async is the default") {
  JournalSync mode = JournalSync::Fdatasync;
  CHECK(parse_journal_sync("async", mode));
  CHECK(mode == JournalSync::Async);
  CHECK(parse_journal_sync("fdatasync", mode));
  CHECK(mode == JournalSync::Fdatasync);
  CHECK_FALSE(parse_journal_sync("sometimes", mode));
  CHECK(mode == JournalSync::Fdatasync);  // unchanged
  CHECK(to_string(JournalSync::Async) == "async");
  CHECK(to_string(JournalSync::Fdatasync) == "fdatasync");
  CHECK(JournalOptions{}.sync == JournalSync::Async);
  CHECK(JournalOptions{}.max_bytes == 0);
}

TEST_CASE("core.journal: a session written with fdatasync reads back whole") {
  const auto dir = fresh_dir("sync");
  const auto path = (dir / "s.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 20);
  {
    JournalOptions o;
    o.sync = JournalSync::Fdatasync;
    JournalFileWriter fw(ring, path, make_info(table), o);
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    write_trades(w, 500);
    CHECK(fw.drain_once() == 500);
    fw.sync();
    fw.stop();
    CHECK_FALSE(fw.failed());
    CHECK(fw.error() == JournalError::Disabled);
  }
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.message_count() == 500);
  CHECK(r.complete());
  CHECK(r.incomplete_reason().empty());
}

TEST_CASE("core.journal: part_path numbers the parts after the configured name") {
  CHECK(JournalFileWriter::part_path("runs/x.fmj", 0) == "runs/x.fmj");
  CHECK(JournalFileWriter::part_path("runs/x.fmj", 1) == "runs/x.1.fmj");
  CHECK(JournalFileWriter::part_path("runs/x.fmj", 12) == "runs/x.12.fmj");
  CHECK(JournalFileWriter::part_path("runs/noext", 3) == "runs/noext.3");
  // A dot in a directory name is not an extension.
  CHECK(JournalFileWriter::part_path("a.b/x", 1) == "a.b/x.1");
}

TEST_CASE("core.journal: journal_max_bytes rolls over and every part is a complete journal") {
  const auto dir = fresh_dir("rotate");
  const auto path = (dir / "s.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 22);
  std::vector<std::string> parts;
  {
    JournalOptions o;
    o.max_bytes = 1 << 20;  // one block
    JournalFileWriter fw(ring, path, make_info(table), o);
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    // Interleaved, as a live session does it: the ring is smaller than the whole session.
    std::size_t drained = 0;
    for (int batch = 0; batch < 8; ++batch) {
      write_trades(w, 5'000);
      drained += fw.drain_once();
    }
    CHECK(drained == 40'000);
    fw.stop();
    CHECK(fw.parts() >= 3);
    parts = fw.part_paths();
    CHECK_FALSE(fw.failed());
  }
  REQUIRE(parts.size() >= 3);
  CHECK(parts[0] == path);
  CHECK(parts[1] == JournalFileWriter::part_path(path, 1));
  std::uint64_t total = 0;
  std::uint64_t last_seq = 0;
  for (const std::string& p : parts) {
    JournalReader r;
    REQUIRE_MESSAGE(r.open(p), "cannot open " << p);
    // Each part repeats the header and the instrument table.
    CHECK(r.header().session_id == 7);
    REQUIRE(r.instruments().size() == 1);
    CHECK(r.instruments()[0].symbol == "BTCUSDT");
    CHECK(r.complete());
    total += r.message_count();
    CHECK(r.last_seq() > last_seq);
    last_seq = r.last_seq();
  }
  CHECK(total == 40'000);
}

TEST_CASE("core.journal: retention removes journals older than the limit and keeps the rest") {
  const auto dir = fresh_dir("retention");
  const auto old_file = dir / "old.fmj";
  const auto new_file = dir / "new.fmj";
  const auto other = dir / "keep.txt";
  for (const auto& p : {old_file, new_file, other}) {
    std::ofstream out(p);
    out << "x";
  }
  const auto now = std::filesystem::file_time_type::clock::now();
  std::filesystem::last_write_time(old_file, now - std::chrono::hours(24 * 10));
  std::string error;
  CHECK(prune_journals(dir.string(), 7, &error) == 1);
  CHECK(error.empty());
  CHECK_FALSE(std::filesystem::exists(old_file));
  CHECK(std::filesystem::exists(new_file));
  CHECK(std::filesystem::exists(other));  // only .fmj files are removed
  // 0 keeps everything, and a directory that is not there is not an error.
  std::filesystem::last_write_time(new_file, now - std::chrono::hours(24 * 100));
  CHECK(prune_journals(dir.string(), 0) == 0);
  CHECK(std::filesystem::exists(new_file));
  CHECK(prune_journals((dir / "missing").string(), 7) == 0);
}

TEST_CASE("core.journal: a file with no trailer is incomplete and says why") {
  const auto dir = fresh_dir("incomplete");
  const auto path = (dir / "s.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 20);
  {
    JournalFileWriter fw(ring, path, make_info(table));
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    write_trades(w, 100);
    CHECK(fw.drain_once() == 100);
    fw.flush_block();
    fw.sync();
    // No stop(): the file keeps its preallocated tail and never gets a trailer, which is what a
    // process killed mid-session leaves behind.
  }
  // The destructor of a writer that was never stopped still closes cleanly, so truncate the
  // trailer away by hand to get the shape a crash leaves.
  {
    JournalReader r;
    REQUIRE(r.open(path));
    CHECK(r.has_trailer());
  }
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - sizeof(JournalBlockHeader));
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.message_count() == 100);
  CHECK_FALSE(r.complete());
  CHECK_FALSE(r.has_trailer());
  CHECK(r.incomplete_reason() == "it has no trailer, so the writer did not close it");
}

TEST_CASE("core.journal: a truncated tail block is incomplete and says why") {
  const auto dir = fresh_dir("truncated");
  const auto path = (dir / "s.fmj").string();
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 22);
  {
    JournalFileWriter fw(ring, path, make_info(table));
    REQUIRE(fw.ok());
    JournalWriter w(&ring);
    write_trades(w, 20'000);
    CHECK(fw.drain_once() == 20'000);
    fw.stop();
  }
  const auto size = std::filesystem::file_size(path);
  std::filesystem::resize_file(path, size - 4096);  // cut into the last block
  JournalReader r;
  REQUIRE(r.open(path));
  CHECK(r.truncated_tail());
  CHECK_FALSE(r.complete());
  CHECK(r.incomplete_reason() == "its last block is truncated or corrupt");
  CHECK(r.message_count() > 0);  // the blocks before the cut are still readable
}

TEST_CASE("core.journal: a writer that cannot open its file reports it and stays failed") {
  const InstrumentTable table = make_table();
  MsgRing ring(1 << 16);
  JournalFileWriter fw(ring, "/does/not/exist/s.fmj", make_info(table));
  CHECK_FALSE(fw.ok());
  CHECK(fw.failed());
  CHECK(fw.error() == JournalError::OpenFailed);
  CHECK_FALSE(fw.open_error());
  CHECK(to_string(JournalError::NoSpace) == "NoSpace");
}
