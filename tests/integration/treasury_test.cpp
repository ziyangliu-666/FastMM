// Pool treasury end to end: core/treasury.hpp plans, live/treasury_thread.hpp drives it through the
// pool's Binance connectors, and the transfers go over HTTP to an in-process master account that
// checks the key and the signature, keeps every account's wallets and answers the transfer history
// by client id. What the engine would journal arrives on the treasury ring.
#include "../venues/fake_venue_util.hpp"

#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/live/treasury_thread.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/binance/binance_venue.hpp"
#include "fastmm/venues/binance_usdm/binance_usdm_venue.hpp"

#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::test;

namespace {

constexpr std::int64_t kS = 1'000'000'000;
constexpr const char* kKey = "master-key";
constexpr const char* kSecret = "master-secret";

std::string decode(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '%' && i + 2 < s.size()) {
      out += static_cast<char>(std::stoi(std::string(s.substr(i + 1, 2)), nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// The master account: wallets per (email, account type), universalTransfer and its history.
class MasterAccount {
 public:
  struct Row {
    std::string tran_id;
    std::string from_email;
    std::string status;
  };

  MasterAccount() {
    srv_.route("POST", "/sapi/v1/sub-account/universalTransfer", [this](const net::HttpRequest& r) {
      return post(r);
    });
    srv_.route("GET", "/sapi/v1/sub-account/universalTransfer", [this](const net::HttpRequest& r) {
      return get(r);
    });
    srv_.start();
  }

  [[nodiscard]] std::string url() const { return srv_.http_base(); }

  void set(const std::string& email, const std::string& type, const char* amount) {
    const std::lock_guard<std::mutex> l(mu_);
    wallets_[{email, type}] = *Notional::parse(amount);
  }
  [[nodiscard]] Notional balance(const std::string& email, const std::string& type) {
    const std::lock_guard<std::mutex> l(mu_);
    return wallets_[{email, type}];
  }
  [[nodiscard]] int posts() {
    const std::lock_guard<std::mutex> l(mu_);
    return posts_;
  }
  // The next POST is carried out and answered with a 503, as a lost answer looks.
  void lose_next_answer() {
    const std::lock_guard<std::mutex> l(mu_);
    lose_next_ = true;
  }

 private:
  bool signed_ok(const net::HttpRequest& r) {
    const std::string_view q = r.query;
    const std::size_t at = q.rfind("&signature=");
    if (at == std::string_view::npos || r.header("X-MBX-APIKEY") != kKey) return false;
    binance::Credentials c;
    c.api_key = kKey;
    c.secret.value = kSecret;
    return binance::Signer(c).sign(q.substr(0, at)) == q.substr(at + 11);
  }

  net::HttpServerResponse post(const net::HttpRequest& r) {
    const std::lock_guard<std::mutex> l(mu_);
    ++posts_;
    if (!signed_ok(r))
      return net::HttpServerResponse::json(400, R"({"code":-1022,"msg":"Signature invalid"})");
    const std::string id = decode(r.param("clientTranId"));
    if (const auto it = rows_.find(id); it != rows_.end())
      return net::HttpServerResponse::json(200, R"({"tranId":)" + it->second.tran_id + "}");
    const std::string from = decode(r.param("fromEmail"));
    const std::string to = decode(r.param("toEmail"));
    const std::string from_type(r.param("fromAccountType"));
    const std::string to_type(r.param("toAccountType"));
    const Notional amount = *Notional::parse(r.param("amount"));
    Notional& src = wallets_[{from, from_type}];
    if (src < amount)
      return net::HttpServerResponse::json(400, R"({"code":-9000,"msg":"Insufficient balance"})");
    src = src - amount;
    wallets_[{to, to_type}] = wallets_[{to, to_type}] + amount;
    const std::string tran = std::to_string(1000 + rows_.size());
    rows_[id] = Row{tran, from, "SUCCESS"};
    if (lose_next_) {
      lose_next_ = false;
      return net::HttpServerResponse::json(503, "");
    }
    return net::HttpServerResponse::json(200, R"({"tranId":)" + tran + "}");
  }

  net::HttpServerResponse get(const net::HttpRequest& r) {
    const std::lock_guard<std::mutex> l(mu_);
    if (!signed_ok(r))
      return net::HttpServerResponse::json(400, R"({"code":-1022,"msg":"Signature invalid"})");
    const std::string id = decode(r.param("clientTranId"));
    const std::string from = decode(r.param("fromEmail"));
    const auto it = rows_.find(id);
    if (it == rows_.end() || it->second.from_email != from)
      return net::HttpServerResponse::json(200, R"({"result":[],"totalCount":0})");
    return net::HttpServerResponse::json(200,
                                         R"({"result":[{"tranId":)" + it->second.tran_id +
                                             R"(,"clientTranId":")" + id + R"(","status":")" +
                                             it->second.status + R"("}],"totalCount":1})");
  }

  FakeVenueServer srv_;
  std::mutex mu_;
  std::map<std::pair<std::string, std::string>, Notional> wallets_;
  std::map<std::string, Row> rows_;
  int posts_ = 0;
  bool lose_next_ = false;
};

// A pool of two accounts of one kind: the primary holds the transfer key.
struct TestPool {
  std::string type;  // SPOT | USDT_FUTURE
  std::vector<std::unique_ptr<Venue>> venues;
  std::vector<std::string> emails{"a@example.com", "b@example.com"};

  TestPool(const std::string& kind, const std::string& transfer_url) {
    ::setenv("FASTMM_TEST_MASTER_KEY", kKey, 1);
    ::setenv("FASTMM_TEST_MASTER_SECRET", kSecret, 1);
    type = kind == "binance_usdm" ? "USDT_FUTURE" : "SPOT";
    for (std::size_t i = 0; i < 2; ++i) {
      VenueSection s;
      s.name = i == 0 ? "main" : "second";
      s.kind = kind;
      s.api_key = "trade-key";
      s.api_secret = "trade-secret";
      s.extra["sub_account_email"] = emails[i];
      if (i == 0) {
        s.extra["transfer_api_key_env"] = "FASTMM_TEST_MASTER_KEY";
        s.extra["transfer_api_secret_env"] = "FASTMM_TEST_MASTER_SECRET";
        s.extra["transfer_rest_url"] = transfer_url;
      }
      if (kind == "binance_usdm") {
        s.ws_url = "wss://fstream.binancefuture.com";
        s.rest_url = "https://testnet.binancefuture.com";
        venues.push_back(std::make_unique<binance_usdm::BinanceUsdmVenue>(
            VenueId{static_cast<std::uint8_t>(i)},
            binance_usdm::make_binance_usdm_config(s, false)));
      } else {
        binance::VenueSectionView v;
        v.name = s.name;
        v.ws_url = "wss://stream.testnet.binance.vision";
        v.rest_url = "https://testnet.binance.vision";
        v.api_key = s.api_key;
        v.api_secret = s.api_secret;
        v.extra = &s.extra;
        venues.push_back(std::make_unique<binance::BinanceVenue>(
            VenueId{static_cast<std::uint8_t>(i)}, binance::make_binance_config(v, false)));
      }
    }
  }

  [[nodiscard]] TreasuryConfig config(const std::string& state_file) const {
    TreasuryConfig c;
    c.enabled = true;
    c.asset = "USDT";
    c.members.venue[c.members.count++] = VenueId{0};
    c.members.venue[c.members.count++] = VenueId{1};
    c.names[0] = "main";
    c.names[1] = "second";
    c.interval_ns = 1 * kS;
    c.min_interval_ns = 0;
    c.state_file = state_file;
    return c;
  }
};

struct Harness {
  MasterAccount& master;
  TestPool& pool;
  Harness(MasterAccount& m, TestPool& p) : master(m), pool(p) {}
  MsgRing ring{1U << 16};
  std::vector<VenueId> refreshed;
  std::int64_t now = 1'790'000'000 * kS;
  // The free balances the engine reports, when they are not the wallets'.
  std::vector<const char*> view;

  std::unique_ptr<live::TreasuryThread> make(const std::string& state_file) {
    auto t = std::make_unique<Treasury>(pool.config(state_file));
    REQUIRE(t->open().has_value());
    auto port = std::make_unique<live::LiveTreasuryPort>(
        VenueId{0},
        [this](VenueId v) -> Venue* {
          return v.value < pool.venues.size() ? pool.venues[v.value].get() : nullptr;
        },
        [this](VenueId v) { refreshed.push_back(v); },
        &ring,
        [] {});
    std::vector<live::TreasuryThread::Pool> pools;
    pools.push_back({std::move(t), std::move(port)});
    // The engine's balance table: the master account's wallets, reported now.
    return std::make_unique<live::TreasuryThread>(
        std::move(pools),
        [this](std::vector<TreasuryBalance>& out) {
          for (std::uint8_t i = 0; i < 2; ++i) {
            TreasuryBalance b;
            b.venue = VenueId{i};
            b.asset.assign("USDT");
            b.free = view.empty() ? master.balance(pool.emails[i], pool.type)
                                  : *Notional::parse(view[i]);
            b.known = true;
            b.as_of_ns = now;
            out.push_back(b);
          }
        },
        [this] { return now; });
  }

  // The records on the ring, by TreasuryEventKind.
  std::vector<TreasuryEventKind> records() {
    std::vector<TreasuryEventKind> out;
    while (const std::byte* p = ring.try_peek()) {
      const auto& m = *reinterpret_cast<const ControlTransferMsg*>(p);
      CHECK(m.hdr.type == EventType::Control);
      CHECK(m.command == ControlCommand::Transfer);
      CHECK(m.hdr.venue == VenueId{0});
      CHECK(m.asset.view() == "USDT");
      out.push_back(static_cast<TreasuryEventKind>(m.event));
      ring.release();
    }
    return out;
  }
};

std::string ledger_path(const char* name) {
  const auto p = fastmm::test::tmp_dir() / name;
  std::filesystem::remove(p);
  return p.string();
}

}  // namespace

TEST_CASE("integration.treasury: a spot pool's short account is topped up over HTTP") {
  MasterAccount master;
  master.set("a@example.com", "SPOT", "1000");
  master.set("b@example.com", "SPOT", "0");
  TestPool pool("binance_spot", master.url());
  CHECK(pool.venues[0]->caps().internal_transfer);
  CHECK_FALSE(pool.venues[1]->caps().internal_transfer);  // no key: it only names its account
  Harness h(master, pool);
  auto t = h.make(ledger_path("treasury_spot.ledger"));

  t->step();  // plans 500 main -> second and sends it
  CHECK(master.posts() == 1);
  CHECK(master.balance("a@example.com", "SPOT") == *Notional::parse("500"));
  CHECK(master.balance("b@example.com", "SPOT") == *Notional::parse("500"));
  CHECK(t->stats().sent == 1);
  CHECK(t->stats().in_flight == 1);

  h.now += 1 * kS;
  t->step();  // the history says SUCCESS
  CHECK(t->stats().done == 1);
  CHECK(t->stats().in_flight == 0);
  CHECK(h.refreshed == std::vector<VenueId>{VenueId{0}, VenueId{1}});
  CHECK(h.records() ==
        std::vector<TreasuryEventKind>{TreasuryEventKind::Sent, TreasuryEventKind::Done});

  // Balanced now: nothing more.
  for (int i = 0; i < 5; ++i) {
    h.now += 2 * kS;
    t->step();
  }
  CHECK(master.posts() == 1);
}

TEST_CASE("integration.treasury: a USD-M pool moves futures wallet to futures wallet") {
  MasterAccount master;
  master.set("a@example.com", "USDT_FUTURE", "300");
  master.set("b@example.com", "USDT_FUTURE", "1700");
  TestPool pool("binance_usdm", master.url());
  Harness h(master, pool);
  auto t = h.make(ledger_path("treasury_usdm.ledger"));
  t->step();
  CHECK(master.posts() == 2);  // two legs through the receiver's spot wallet
  CHECK(master.balance("b@example.com", "USDT_FUTURE") == *Notional::parse("1000"));
  CHECK(master.balance("a@example.com", "USDT_FUTURE") == *Notional::parse("1000"));
  CHECK(master.balance("a@example.com", "SPOT") == Notional{});
  h.now += 1 * kS;
  t->step();
  CHECK(t->stats().done == 1);
}

TEST_CASE("integration.treasury: a lost answer and a restart do not send the transfer twice") {
  MasterAccount master;
  master.set("a@example.com", "SPOT", "1000");
  master.set("b@example.com", "SPOT", "0");
  TestPool pool("binance_spot", master.url());
  Harness h(master, pool);
  const std::string ledger = ledger_path("treasury_restart.ledger");
  master.lose_next_answer();
  {
    auto t = h.make(ledger);
    t->step();  // carried out, answered 503: the outcome is unknown here
    CHECK(t->stats().sent == 1);
    CHECK(t->stats().in_flight == 1);
    CHECK(t->stats().errors == 1);
  }  // the process stops
  CHECK(master.balance("b@example.com", "SPOT") == *Notional::parse("500"));
  {
    auto t = h.make(ledger);  // the next process reads the ledger
    CHECK(t->stats().in_flight == 1);
    h.now += 5 * kS;
    t->step();  // asks for it by client id instead of planning anew
    CHECK(t->stats().done == 1);
    CHECK(t->stats().in_flight == 0);
    for (int i = 0; i < 5; ++i) {
      h.now += 2 * kS;
      t->step();
    }
  }
  CHECK(master.posts() == 1);
  CHECK(master.balance("a@example.com", "SPOT") == *Notional::parse("500"));
}

TEST_CASE("integration.treasury: a refused transfer cools the pool down and is not repeated") {
  MasterAccount master;
  master.set("a@example.com", "SPOT", "0");
  master.set("b@example.com", "SPOT", "0");
  TestPool pool("binance_spot", master.url());
  Harness h(master, pool);
  // The engine's view says main holds 1000; the wallet is empty and the venue refuses.
  h.view = {"1000", "0"};
  auto t = h.make(ledger_path("treasury_refused.ledger"));
  t->step();
  CHECK(master.posts() == 1);
  CHECK(t->stats().failed == 1);
  CHECK(t->stats().in_flight == 0);
  for (int i = 0; i < 50; ++i) {
    h.now += 2 * kS;
    t->step();
  }
  CHECK(master.posts() == 1);  // cooldown_s (300 s) holds the next one back
  CHECK(t->stats().limited > 0);
  CHECK(h.records() ==
        std::vector<TreasuryEventKind>{TreasuryEventKind::Sent, TreasuryEventKind::Failed});
}
