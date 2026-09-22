#pragma once
// ItchPublisher: the effects of a MatchingEngine on one instrument, published as TotalView-ITCH
// 5.0 order messages (fastmm-sim-itch, ADR-0015 section 6; the ITCH codec tests).
//
// Publishing rules (what an exchange feed does with each engine effect):
//   order rests                           -> A (or F) with the resting quantity
//   resting order executed                -> E, or C with the execution price (optionally a P
//                                            trade print alongside)
//   resting order cancelled               -> D
//   replace kept in place (same price,    -> X for the reduction; the ITCH reference is kept
//   qty <= leaves: priority kept)            because the queue position is kept
//   replace re-entered, rests untouched   -> U (old reference -> new reference, back of queue)
//                                            with replace_messages, else D + A
//   replace re-entered and traded         -> D for the old order, E for the makers, A for the rest
//
// Order reference numbers and match numbers come from an ItchNumbering shared by every
// publisher of a feed, so they are unique across symbols. Every acknowledged order gets a
// reference number at its ack (whether or not it rests), so an order-entry gateway can return it
// in its acknowledgement.
//
// Driving: engine calls that go through submit() / cancel() / replace() here are published
// completely. A caller that drives the engine directly (MarketGenerator::step, seed_book) calls
// after_call() once the engine returns. The publisher is the engine's MatchingSink; effects on
// accounts other than kGeneratorAccount are also reported to an optional ItchPublisher::Client.
#include "fastmm/codecs/itch/itch_encoder.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/sim/matching_engine.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace fastmm::sim::itch {

// Where the messages go and what time they carry.
class ItchOutput {
 public:
  virtual ~ItchOutput() = default;
  // ITCH Timestamp (nanoseconds since midnight) for the next message.
  virtual std::uint64_t itch_timestamp() noexcept = 0;
  virtual void publish(std::span<const std::byte> msg) noexcept = 0;
};

struct ItchNumbering {
  std::uint64_t next_ref = 0;    // last order reference number assigned
  std::uint64_t next_match = 0;  // last match number assigned
};

struct ItchPublisherOptions {
  std::uint64_t mpid_every = 0;           // every k-th Add Order is F (attribution "FMMM")
  std::uint64_t priced_exec_every = 0;    // every k-th execution is C instead of E
  std::uint64_t non_printable_every = 0;  // every k-th C has Printable = N
  std::uint64_t trade_print_every = 0;    // a P after every k-th execution
  bool replace_messages = false;          // U for a re-entered replace that rests untouched
};

struct ItchPublisherStats {
  std::uint64_t messages = 0;
  std::uint64_t encode_failures = 0;  // a value the encoder refused (the message is dropped)
};

class ItchPublisher final : public MatchingSink {
 public:
  // Effects on accounts other than kGeneratorAccount (an order-entry gateway).
  class Client {
   public:
    virtual ~Client() = default;
    virtual void on_ack(const SimOrder&, std::uint64_t /*ref*/) noexcept {}
    virtual void on_reject(const NewOrder&, RejectReason) noexcept {}
    virtual void on_cancel(const SimOrder&, CancelReason) noexcept {}
    // liquidity: 'A' added (maker), 'R' removed (taker).
    virtual void on_fill(
        const SimOrder&, Price, Qty, char /*liquidity*/, std::uint64_t /*match*/) noexcept {}
  };

  ItchPublisher(ItchOutput& out,
                ItchNumbering& numbering,
                std::uint16_t locate,
                std::string_view symbol,
                const ItchPublisherOptions& opt = {});

  void set_client(Client* c) noexcept { client_ = c; }

  // ---- engine calls, published ----
  SubmitResult submit(MatchingEngine& eng, const NewOrder& o, Timestamp now) noexcept;
  bool cancel(MatchingEngine& eng, AccountId account, ClientOrderId id, Timestamp now) noexcept;
  // MatchingEngine::replace. A replace that keeps priority publishes X for a reduction.
  SubmitResult replace(MatchingEngine& eng,
                       AccountId account,
                       ClientOrderId orig,
                       ClientOrderId new_id,
                       Price price,
                       Qty qty,
                       Timestamp now) noexcept;
  // After a call into `eng` made elsewhere: A for the orders of that call that rest.
  void after_call(const MatchingEngine& eng) noexcept;

  // ---- MatchingSink ----
  void on_ack(const SimOrder& o, Timestamp now) override;
  void on_reject(const NewOrder& n, RejectReason why, Timestamp now) override;
  void on_cancel(const SimOrder& o, CancelReason why, Timestamp now) override;
  void on_fill(const SimOrder& maker, const SimOrder& taker, Price px, Qty qty, Timestamp) override;

  // ITCH reference of a resting engine order (0 if it is not resting).
  [[nodiscard]] std::uint64_t ref_of(std::uint64_t order_id) const noexcept {
    const auto it = resting_.find(order_id);
    return it == resting_.end() ? 0 : it->second.ref;
  }
  [[nodiscard]] std::size_t resting_count() const noexcept { return resting_.size(); }
  [[nodiscard]] std::uint16_t locate() const noexcept { return locate_; }
  [[nodiscard]] std::string_view symbol() const noexcept { return symbol_; }
  [[nodiscard]] const ItchPublisherStats& stats() const noexcept { return stats_; }

 private:
  struct Resting {
    std::uint64_t ref = 0;
    Qty leaves{};
  };
  struct Pending {
    AccountId account = 0;
    ClientOrderId cl_ord_id{};
    std::uint64_t ref = 0;
  };

  void emit(std::size_t n) noexcept;
  void flush_deferred_delete() noexcept;
  void add(const SimOrder& o, std::uint64_t ref) noexcept;

  ItchOutput& out_;
  ItchNumbering& num_;
  std::uint16_t locate_;
  std::string symbol_;
  ItchPublisherOptions opt_;
  Client* client_ = nullptr;
  codecs::itch::ItchEncoder enc_;
  std::array<std::byte, 64> buf_{};
  std::unordered_map<std::uint64_t, Resting> resting_;  // engine order id -> ITCH view
  std::vector<Pending> pending_;                        // acked during the current call
  // replace in progress
  bool in_place_ = false;
  std::uint64_t in_place_ref_ = 0;
  bool reentering_ = false;
  std::uint64_t deferred_delete_ = 0;  // reference of the old leg, D not published yet
  std::uint64_t adds_ = 0;
  std::uint64_t execs_ = 0;
  std::uint64_t priced_ = 0;
  ItchPublisherStats stats_{};
};

}  // namespace fastmm::sim::itch
