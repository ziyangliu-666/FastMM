// JournalSource over a session recorded in parts ([engine] journal_max_bytes): the parts read as
// one stream after the first, or the first alone with parts = false.
#include "backtest_test_util.hpp"

#include "fastmm/backtest/backtest_runner.hpp"
#include "fastmm/backtest/journal_source.hpp"
#include "fastmm/core/journal.hpp"
#include "fastmm/core/msg_ring.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

using namespace fastmm;
using namespace fastmm::bt;
using namespace fastmm::bt::testutil;

namespace {

InstrumentTable one_instrument() {
  InstrumentTable t;
  Instrument i{};
  i.symbol = "BTCUSDT";
  i.tick = px("0.01");
  i.lot = qt("0.001");
  i.flags = Instrument::kEnabled;
  REQUIRE(t.add(i));
  return t;
}

// `n` trades a microsecond apart from t0, as JournalFileWriter rolls them over every MiB (a
// part ends between blocks, so each holds whole 1 MiB blocks of 128-byte trades). Returns the
// parts written.
std::vector<std::string> write_session(const std::string& path,
                                       const InstrumentTable& table,
                                       std::uint64_t session_id,
                                       int n,
                                       std::int64_t t0 = 1'000'000'000) {
  MsgRing ring(1 << 22);
  JournalSessionInfo info;
  info.session_id = session_id;
  info.start_ts = Timestamp{t0};
  info.strategy = "basic_mm";
  info.instruments = &table;
  JournalOptions o;
  o.max_bytes = 1 << 20;
  JournalFileWriter fw(ring, path, info, o);
  REQUIRE(fw.ok());
  JournalWriter w(&ring);
  for (int i = 0; i < n; ++i) {
    TradeMsg t{};
    init_header(t, EventType::Trade, InstrumentId{0}, VenueId{0});
    t.hdr.exch_ts = t.hdr.recv_ts = Timestamp{t0 + 1000 * i};
    t.trade_id = static_cast<std::uint64_t>(i);
    REQUIRE(w.record(t.hdr));
    if (i % 4096 == 4095) fw.drain_once();
  }
  fw.drain_once();
  fw.stop();
  REQUIRE_FALSE(fw.failed());
  return fw.part_paths();
}

// Events of `s`, checking that their time never goes back.
std::size_t count_monotonic(MdSource& s) {
  std::size_t n = 0;
  Timestamp last{};
  while (const EventHeader* h = s.next()) {
    CHECK(h->exch_ts >= last);
    last = h->exch_ts;
    ++n;
  }
  return n;
}

std::filesystem::path fresh_dir(const char* name) {
  const auto d = fastmm::test::tmp_dir() / name;
  std::filesystem::remove_all(d);
  std::filesystem::create_directories(d);
  return d;
}

}  // namespace

TEST_CASE("backtest.journal_parts: the parts of a session read as one stream") {
  const auto dir = fresh_dir("journal_parts");
  const std::string path = (dir / "s.fmj").string();
  const InstrumentTable table = one_instrument();
  const std::vector<std::string> parts = write_session(path, table, 7, 30'000);
  REQUIRE(parts.size() >= 3);
  REQUIRE(parts[0] == path);
  REQUIRE(parts[1] == JournalFileWriter::part_path(path, 1));

  std::size_t each = 0;
  for (const std::string& p : parts) {
    JournalSource one(p, false, false);
    CHECK(one.paths() == std::vector<std::string>{p});
    CHECK(one.md_events() > 0);
    each += one.md_events();
  }
  CHECK(each == 30'000);

  JournalSource all(path);
  CHECK(all.paths() == parts);
  CHECK(all.md_events() == 30'000);
  CHECK(all.seq_gaps() == 0);
  CHECK(all.start_ts() == Timestamp{1'000'000'000});
  CHECK(all.reader().header().session_id == 7);
  CHECK(all.note().find(std::to_string(parts.size()) + " parts") != std::string::npos);
  CHECK(all.note().find("WARNING") == std::string::npos);
  CHECK(count_monotonic(all) == 30'000);
  all.reset();
  CHECK(count_monotonic(all) == 30'000);

  // The spec and the bare path chain too; parts=0 reads the one file.
  std::unique_ptr<MdSource> spec = open_data("journal:" + path, &table);
  CHECK(count_monotonic(*spec) == 30'000);
  std::unique_ptr<MdSource> bare = open_data(path, &table);
  CHECK(count_monotonic(*bare) == 30'000);
  std::unique_ptr<MdSource> first = open_data("journal:" + path + ",parts=0", &table);
  CHECK(count_monotonic(*first) == JournalSource(path, false, false).md_events());
}

TEST_CASE(
    "backtest.journal_parts: a missing part is a gap in the sequence, another session an error") {
  const auto dir = fresh_dir("journal_parts_gap");
  const std::string path = (dir / "s.fmj").string();
  const InstrumentTable table = one_instrument();
  const std::vector<std::string> parts = write_session(path, table, 7, 30'000);
  REQUIRE(parts.size() >= 3);

  // Part 1 lost and the rest renumbered: the parts read, with a warning in the note.
  std::filesystem::remove(parts[1]);
  for (std::size_t k = 2; k < parts.size(); ++k) std::filesystem::rename(parts[k], parts[k - 1]);
  JournalSource gap(path);
  CHECK(gap.paths().size() == parts.size() - 1);
  CHECK(gap.seq_gaps() == 1);
  CHECK(gap.note().find("WARNING") != std::string::npos);
  CHECK(gap.note().find(parts[1]) != std::string::npos);
  CHECK(count_monotonic(gap) == gap.md_events());
  CHECK(gap.md_events() < 30'000);

  // A file numbered like the next part but of another session is refused.
  const std::string other = JournalFileWriter::part_path(path, parts.size() - 1);
  write_session(other, table, 8, 10);
  CHECK_THROWS_WITH_AS(
      JournalSource{path}, doctest::Contains("session 8, not 7"), std::runtime_error);
  CHECK(JournalSource(path, false, false).paths().size() == 1);
}
