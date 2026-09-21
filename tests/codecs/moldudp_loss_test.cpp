// MoldUDP64 loss injection (plan 7, ADR-0015): a Transmitter publishes a message stream to one
// or two lines (A/B), each of which independently drops, duplicates and reorders datagrams.
// Retransmission responses, when there is a re-request server, arrive on a third line and are
// lossy too. The Receiver must hand the handler each message at most once, in sequence order,
// with the published contents. With a re-request server every message arrives; without one,
// every missing message is reported through on_gap_unrecoverable() exactly once, and only
// messages that no line delivered promptly may be missing.
#include "nasdaq_test_util.hpp"

#include "fastmm/codecs/moldudp/moldudp64.hpp"

#include <array>
#include <deque>
#include <random>

using namespace fastmm;
using namespace fastmm::codecs;
using namespace fastmm::codecs::moldudp;
using fastmm::codecs::test::Bytes;

namespace {
struct Handler {
  std::vector<std::pair<std::uint64_t, Bytes>> got;
  std::vector<std::pair<std::uint64_t, std::uint64_t>> lost;
  std::vector<Bytes> requests;
  bool eos = false;
  void on_message(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
    got.emplace_back(seq, Bytes(msg.begin(), msg.end()));
  }
  void send_request(std::span<const std::byte> packet) noexcept {
    requests.emplace_back(packet.begin(), packet.end());
  }
  void on_end_of_session() noexcept { eos = true; }
  void on_gap_unrecoverable(std::uint64_t from, std::uint64_t count) noexcept {
    lost.emplace_back(from, count);
  }
};

Bytes make_message(std::uint64_t seq, std::size_t len) {
  Bytes m(len);
  for (std::size_t i = 0; i < len; ++i) m[i] = static_cast<std::byte>((seq * 131 + i * 7) & 0xFF);
  return m;
}

struct Totals {
  std::uint64_t dropped = 0;
  std::uint64_t gaps = 0;
  std::uint64_t lost = 0;
  std::uint64_t duplicates = 0;
  std::uint64_t held = 0;
  std::uint64_t skew_samples = 0;
};

constexpr std::uint64_t kMessages = 3000;

// One seeded run. lines: 1 or 2 multicast lines; retransmit: a re-request server exists.
void run(std::uint64_t seed, std::size_t lines, bool retransmit, Totals& totals) {
  CAPTURE(seed);
  CAPTURE(lines);
  CAPTURE(retransmit);
  std::mt19937_64 rng(seed);
  auto chance = [&](double p) { return std::uniform_real_distribution<double>(0, 1)(rng) < p; };
  auto uni = [&](std::uint64_t lo, std::uint64_t hi) {
    return std::uniform_int_distribution<std::uint64_t>(lo, hi)(rng);
  };

  TransmitterConfig tcfg;
  tcfg.max_datagram = 300;
  tcfg.history_messages = 8192;
  Transmitter tx("LOSS" + std::to_string(seed), tcfg);
  Handler h;
  ReceiverConfig rcfg;
  rcfg.next_sequence = 1;
  rcfg.request_timeout_ns = 5'000'000;
  rcfg.max_request_count = 64;
  rcfg.max_request_attempts = 0;  // retransmissions are lossy: keep asking
  rcfg.gap_timeout_ns = 50'000;
  rcfg.can_request = retransmit;
  Receiver<Handler> rx(h, rcfg);

  constexpr std::int64_t kStepNs = 20'000;
  constexpr std::size_t kRetransLine = 2;
  std::vector<Bytes> published;
  std::vector<bool> prompt(kMessages + 1, false);  // a copy reached a line without delay
  std::array<std::deque<Bytes>, 3> inflight;
  std::array<std::deque<Bytes>, 2> delayed;  // reordered datagrams, released later
  std::array<std::byte, 1500> buf{};
  std::int64_t now = 0;

  auto mark_prompt = [&](const Bytes& d) {
    PacketView p;
    REQUIRE(parse_packet(codecs::test::span_of(d), p));
    for (std::uint64_t s = p.sequence; s < p.next_sequence() && s <= kMessages; ++s)
      prompt[s] = true;
  };
  auto multicast = [&](const Bytes& d) {
    for (std::size_t l = 0; l < lines; ++l) {
      if (chance(0.15)) {
        ++totals.dropped;
        continue;
      }
      if (chance(0.05)) {
        delayed[l].push_back(d);
        continue;
      }
      mark_prompt(d);
      inflight[l].push_back(d);
      if (chance(0.05)) inflight[l].push_back(d);  // duplicate
    }
  };

  for (int iter = 0; iter < 200'000; ++iter) {
    now += kStepNs;
    // Retransmissions answered in the previous iteration arrive first.
    std::deque<Bytes> answers;
    answers.swap(inflight[kRetransLine]);
    if (published.size() < kMessages) {
      const std::uint64_t burst = uni(1, 12);
      for (std::uint64_t i = 0; i < burst && published.size() < kMessages; ++i) {
        const std::uint64_t seq = published.size() + 1;
        published.push_back(make_message(seq, uni(0, 40)));
        REQUIRE(tx.publish(codecs::test::span_of(published.back())) == seq);
      }
      while (std::size_t n = tx.next_packet(buf))
        multicast(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)));
    } else if (iter % 10 == 0) {
      const std::size_t n =
          rx.next_sequence() > kMessages ? tx.end_of_session(buf) : tx.heartbeat(buf);
      multicast(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)));
    }
    for (std::size_t l = 0; l < lines; ++l) {
      if (!delayed[l].empty() && chance(0.3)) {
        inflight[l].push_back(delayed[l].front());
        delayed[l].pop_front();
      }
    }
    // Deliver: retransmissions, then the lines interleaved at random, 100 ns apart.
    std::int64_t t = now;
    for (const Bytes& d : answers) rx.on_packet(kRetransLine, codecs::test::span_of(d), t += 100);
    for (;;) {
      std::array<std::size_t, 2> ready{};
      std::size_t n_ready = 0;
      for (std::size_t l = 0; l < lines; ++l) {
        if (!inflight[l].empty()) ready[n_ready++] = l;
      }
      if (n_ready == 0) break;
      const std::size_t l = ready[uni(0, n_ready - 1)];
      const Bytes d = inflight[l].front();
      inflight[l].pop_front();
      rx.on_packet(l, codecs::test::span_of(d), t += 100);
    }
    rx.on_timer(t += 100);
    // Answer retransmission requests (outside the receiver's callbacks); lossy.
    std::vector<Bytes> reqs;
    reqs.swap(h.requests);
    for (const Bytes& r : reqs) {
      const std::size_t n = tx.answer_request(codecs::test::span_of(r), buf);
      if (n != 0 && !chance(0.10))
        inflight[kRetransLine].emplace_back(buf.begin(),
                                            buf.begin() + static_cast<std::ptrdiff_t>(n));
    }
    if (h.eos && published.size() == kMessages) break;
  }

  // Each sequence exactly once: delivered (in order, with its contents) or reported lost.
  std::vector<int> seen(kMessages + 1, 0);
  std::uint64_t prev = 0;
  for (const auto& [seq, msg] : h.got) {
    REQUIRE(seq > prev);
    REQUIRE(seq <= kMessages);
    REQUIRE(msg == published[seq - 1]);
    ++seen[seq];
    prev = seq;
  }
  std::uint64_t lost = 0;
  for (const auto& [from, count] : h.lost) {
    for (std::uint64_t s = from; s < from + count; ++s) {
      REQUIRE(s <= kMessages);
      REQUIRE_FALSE(prompt[s]);  // a promptly delivered copy is never given up
      ++seen[s];
      ++lost;
    }
  }
  for (std::uint64_t s = 1; s <= kMessages; ++s) REQUIRE(seen[s] == 1);

  CHECK(h.eos);
  CHECK(rx.state() == SessionState::Up);
  CHECK(rx.stats().messages == h.got.size());
  CHECK(rx.stats().lost_messages == lost);
  if (retransmit) {
    CHECK(h.lost.empty());
    CHECK(h.got.size() == kMessages);
    CHECK(rx.stats().requests >= rx.stats().gaps);
  } else {
    CHECK(rx.stats().requests == 0);
  }
  totals.gaps += rx.stats().gaps;
  totals.lost += lost;
  totals.duplicates += rx.stats().duplicate_packets;
  totals.held += rx.stats().held_packets;
  for (const ReceiverLineStats& l : rx.stats().lines) totals.skew_samples += l.skew_samples;
}
}  // namespace

TEST_CASE("codecs.moldudp: packet loss on one line is recovered with in-order delivery") {
  Totals t;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) run(seed, 1, true, t);
  CHECK(t.dropped > 0);
  CHECK(t.gaps > 0);
  CHECK(t.held > 0);
}

TEST_CASE("codecs.moldudp: A and B lines with independent loss deliver every message once") {
  Totals t;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) run(seed, 2, true, t);
  CHECK(t.dropped > 0);
  CHECK(t.gaps > 0);  // both lines lost the same packet
  CHECK(t.duplicates > 0);
  CHECK(t.skew_samples > 0);
}

TEST_CASE("codecs.moldudp: A and B lines without retransmission report what both lines lost") {
  Totals t;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) run(seed, 2, false, t);
  CHECK(t.dropped > 0);
  CHECK(t.lost > 0);
  CHECK(t.lost < 20 * kMessages / 10);
}
