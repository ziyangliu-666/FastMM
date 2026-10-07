#pragma once
// PaperVenue: what a dry run (fastmm-live --dry-run) puts in front of each connector. The connector
// keeps its public market data; the engine quotes as it would live, and its orders end here instead
// of on the wire. Every new order is acknowledged at once and rests until the engine cancels or
// replaces it (an IOC or market order expires unfilled), so the strategy, the quote manager, the
// risk checks and the rate limits run exactly as in a keyed session, and nothing ever fills.
//
// The engine does not know: nothing is added to its hot path. The paper orders are what the
// session reports in a dry run (the desired quotes per instrument and the counts of would-be new
// orders, cancels and replaces).
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/venues/symbology.hpp"
#include "fastmm/venues/venue.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace fastmm::live {

// What the engine would have sent, for one instrument (PaperVenue::quotes).
struct PaperQuotes {
  InstrumentId instrument{};
  Level bid{};  // the best resting bid (price, and the quantity at that price); zero: none
  Level ask{};
  std::uint32_t bid_orders = 0;  // resting paper orders per side
  std::uint32_t ask_orders = 0;
  std::uint64_t news = 0;  // would-be new orders, cancels and replaces since the start
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
};

struct PaperTotals {
  std::uint64_t news = 0;
  std::uint64_t cancels = 0;
  std::uint64_t replaces = 0;
};

class PaperVenue final : public venues::Venue {
 public:
  // `replace`: the connector can amend in place when keyed (VenueCapabilities::replace), which the
  // dry-run instance does not report.
  PaperVenue(std::unique_ptr<venues::Venue> inner, bool replace);

  [[nodiscard]] VenueId id() const noexcept override { return inner_->id(); }
  [[nodiscard]] std::string_view name() const noexcept override { return inner_->name(); }
  [[nodiscard]] venues::VenueCaps caps() const noexcept override;
  Result<void, std::string> load_reference_data(InstrumentTable& instruments) override {
    return inner_->load_reference_data(instruments);
  }
  Result<std::vector<venues::VenueFee>, std::string> account_fees(
      const InstrumentTable& instruments) override {
    return inner_->account_fees(instruments);
  }
  [[nodiscard]] bool refused_account_settings() const noexcept override {
    return inner_->refused_account_settings();
  }
  void attach(const venues::SymbolTable& symbols,
              const InstrumentTable& instruments,
              venues::EventSink& md_sink,
              venues::EventSink& order_sink,
              MsgRing* outbound) override;
  void connect(net::Reactor& reactor) override { inner_->connect(reactor); }
  void disconnect() override { inner_->disconnect(); }
  void subscribe(std::span<const InstrumentId> instruments) override {
    inner_->subscribe(instruments);
  }
  void on_timer(std::int64_t now_ns) override { inner_->on_timer(now_ns); }
  void on_wake() override;
  void send_now(std::span<const EventHeader* const> batch) override;
  void poll() noexcept override { inner_->poll(); }
  void resync_books() override { inner_->resync_books(); }
  void request_open_orders() override;
  bool cancel_all() override;
  [[nodiscard]] venues::VenueStatus status() const noexcept override { return inner_->status(); }

  // The instruments this venue has had paper orders for, in id order. Any thread.
  [[nodiscard]] std::vector<PaperQuotes> quotes() const;
  [[nodiscard]] PaperTotals totals() const;

 private:
  struct Order {
    InstrumentId instrument;
    Side side;
    Price price;
    Qty qty;
  };
  struct Counts {
    std::uint64_t news = 0;
    std::uint64_t cancels = 0;
    std::uint64_t replaces = 0;
  };

  void handle(const EventHeader& h);  // under mu_
  void ack(InstrumentId inst, ClientOrderId id, std::uint64_t venue_id);
  void expire(InstrumentId inst, ClientOrderId id, std::uint64_t venue_id);
  void reconcile_snapshot();  // under mu_

  std::unique_ptr<venues::Venue> inner_;
  bool replace_;
  const InstrumentTable* instruments_ = nullptr;
  venues::EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  MsgRing inner_outbound_{1U << 12};  // the connector's: never written
  mutable std::mutex mu_;             // the network thread and the session's reports
  std::unordered_map<std::uint64_t, Order> orders_;
  std::unordered_map<std::uint32_t, Counts> counts_;
  std::uint64_t next_venue_id_ = 0;
};

}  // namespace fastmm::live
