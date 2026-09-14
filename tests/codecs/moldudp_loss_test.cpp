// MoldUDP64 loss injection (plan 7): a Transmitter publishes a message stream through a lossy
// channel that drops, duplicates and reorders datagrams (retransmission responses are lossy
// too). The Receiver must detect every gap, recover it from the Transmitter's history and hand
// the handler each message exactly once, in sequence order, with the published contents.
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
  std::vector<Bytes> requests;
  bool eos = false;
  void on_message(std::uint64_t seq, std::span<const std::byte> msg) noexcept {
    got.emplace_back(seq, Bytes(msg.begin(), msg.end()));
  }
  void send_request(std::span<const std::byte> packet) noexcept {
    requests.emplace_back(packet.begin(), packet.end());
  }
  void on_end_of_session() noexcept { eos = true; }
};

Bytes make_message(std::uint64_t seq, std::size_t len) {
  Bytes m(len);
  for (std::size_t i = 0; i < len; ++i) m[i] = static_cast<std::byte>((seq * 131 + i * 7) & 0xFF);
  return m;
}
}  // namespace

TEST_CASE("codecs.moldudp: packet loss is detected and recovered with in-order delivery") {
  std::uint64_t total_gaps = 0;
  std::uint64_t total_dropped = 0;
  for (std::uint64_t seed = 1; seed <= 20; ++seed) {
    CAPTURE(seed);
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
    Receiver<Handler> rx(h, rcfg);

    constexpr std::uint64_t kMessages = 3000;
    std::vector<Bytes> published;
    std::deque<Bytes> delayed;  // reordered datagrams, released later
    std::deque<Bytes> inflight;
    std::array<std::byte, 1500> buf{};
    std::int64_t now = 0;
    std::uint64_t dropped = 0;

    auto channel = [&](const Bytes& d, double loss) {
      if (chance(loss)) {
        ++dropped;
        return;
      }
      if (chance(0.05)) {
        delayed.push_back(d);
        return;
      }
      inflight.push_back(d);
      if (chance(0.05)) inflight.push_back(d);  // duplicate
    };

    for (int iter = 0; iter < 200'000; ++iter) {
      now += 1'000'000;
      // publish a burst while the stream lasts
      if (published.size() < kMessages) {
        const std::uint64_t burst = uni(1, 12);
        for (std::uint64_t i = 0; i < burst && published.size() < kMessages; ++i) {
          const std::uint64_t seq = published.size() + 1;
          published.push_back(make_message(seq, uni(0, 40)));
          REQUIRE(tx.publish(codecs::test::span_of(published.back())) == seq);
        }
        while (std::size_t n = tx.next_packet(buf))
          channel(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)), 0.15);
      } else if (iter % 10 == 0) {
        const std::size_t n =
            h.got.size() == kMessages ? tx.end_of_session(buf) : tx.heartbeat(buf);
        channel(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)), 0.15);
      }
      if (!delayed.empty() && chance(0.3)) {
        inflight.push_back(delayed.front());
        delayed.pop_front();
      }
      // deliver what is in flight
      while (!inflight.empty()) {
        const Bytes d = inflight.front();
        inflight.pop_front();
        rx.on_packet(codecs::test::span_of(d));
      }
      rx.on_timer(now);
      // answer retransmission requests (outside the receiver's callbacks)
      std::vector<Bytes> reqs;
      reqs.swap(h.requests);
      for (const Bytes& r : reqs) {
        const std::size_t n = tx.answer_request(codecs::test::span_of(r), buf);
        if (n != 0) channel(Bytes(buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(n)), 0.10);
      }
      if (h.eos && published.size() == kMessages) break;
    }

    REQUIRE(h.got.size() == kMessages);
    for (std::uint64_t i = 0; i < kMessages; ++i) {
      REQUIRE(h.got[i].first == i + 1);
      REQUIRE(h.got[i].second == published[i]);
    }
    CHECK(h.eos);
    CHECK(rx.state() == SessionState::Up);
    CHECK(rx.stats().gaps > 0);
    CHECK(rx.stats().requests >= rx.stats().gaps);
    CHECK(rx.stats().messages == kMessages);
    total_gaps += rx.stats().gaps;
    total_dropped += dropped;
  }
  CHECK(total_dropped > 0);
  CHECK(total_gaps > 0);
}
