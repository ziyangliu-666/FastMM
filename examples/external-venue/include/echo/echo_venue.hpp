#pragma once
// A venue connector in a project of its own, built against the installed FastMM headers. It is
// deliberately the smallest thing that is still a venue: it publishes one quote per subscribed
// instrument from its own `[venues.<name>]` keys and refuses every order. Copy this directory to
// start a real connector and follow docs/how-to/venues/add-a-venue.md.
//
// Everything the core learns about this venue comes from the registry entry in src/register.cpp:
// the `kind` that selects it, what it can do, and the config keys it owns.
#include <cstdint>
#include <fastmm/config/config.hpp>
#include <fastmm/venues/event_sink.hpp>
#include <fastmm/venues/registry.hpp>
#include <fastmm/venues/venue.hpp>
#include <memory>
#include <string>
#include <vector>

namespace echo {

using fastmm::InstrumentId;
using fastmm::InstrumentTable;
using fastmm::MsgRing;
using fastmm::Price;
using fastmm::Qty;
using fastmm::Result;
using fastmm::VenueId;
using fastmm::venues::EventSink;
using fastmm::venues::SymbolTable;
using fastmm::venues::Venue;
using fastmm::venues::VenueCaps;
using fastmm::venues::VenueStatus;

// What src/register.cpp reads out of the `[venues.<name>]` section. A connector's configuration is
// its own: the central schema knows none of these keys.
struct EchoVenueConfig {
  std::string name = "echo";
  Price bid = Price::from_int(100);
  Price ask = Price::from_int(101);
  Qty qty = Qty::from_int(1);
  bool dry_run = false;
};

class EchoVenue final : public Venue {
 public:
  EchoVenue(VenueId id, EchoVenueConfig cfg) : id_(id), cfg_(std::move(cfg)) {}

  [[nodiscard]] VenueId id() const noexcept override { return id_; }
  [[nodiscard]] std::string_view name() const noexcept override { return cfg_.name; }
  [[nodiscard]] VenueCaps caps() const noexcept override;

  Result<void, std::string> load_reference_data(InstrumentTable&) override { return {}; }
  void attach(const SymbolTable& symbols,
              const InstrumentTable& instruments,
              EventSink& md_sink,
              EventSink& order_sink,
              MsgRing* outbound) override;

  void connect(fastmm::net::Reactor&) override;
  void disconnect() override { connected_ = false; }
  void subscribe(std::span<const InstrumentId> instruments) override;
  void on_timer(std::int64_t) override {}
  void on_wake() override;
  void send_now(std::span<const fastmm::EventHeader* const> batch) override;
  void request_open_orders() override;
  bool cancel_all() override { return true; }
  [[nodiscard]] VenueStatus status() const noexcept override { return status_; }

 private:
  void publish();
  void refuse(const fastmm::EventHeader& h);

  VenueId id_;
  EchoVenueConfig cfg_;
  std::vector<InstrumentId> subscribed_;
  EventSink* md_sink_ = nullptr;
  EventSink* order_sink_ = nullptr;
  MsgRing* outbound_ = nullptr;
  VenueStatus status_{};
  bool connected_ = false;
};

// Registers `kind = "echo"`. Call it once, before the session starts; the app in apps/live.cpp
// does it in main().
void register_echo_venue(
    fastmm::venues::VenueRegistry& r = fastmm::venues::VenueRegistry::instance());

}  // namespace echo
