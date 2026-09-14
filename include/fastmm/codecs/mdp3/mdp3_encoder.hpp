#pragma once
// MDP 3.0 encoding for tests, simulation and benchmarks (plan 7). CME is the only publisher of
// real MDP 3.0; this side exists to drive Mdp3Decoder / Mdp3Feed from the simulator and to
// build fixtures.
//
//   PacketBuilder   packet header + messages written with the generated schema writers
//   encode_*        one message per call (false and the packet unchanged if it does not fit)
//   MbpPublisher    turns successive top-N views of one book side into CME-style MBP entries
//                   (New / Change / Delete by MDPriceLevel) with a per-instrument RptSeq
#include "fastmm/codecs/mdp3/generated/mdp3_schema.hpp"
#include "fastmm/codecs/mdp3/mdp3_instruments.hpp"
#include "fastmm/codecs/mdp3/mdp3_packet.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/messages.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace fastmm::codecs::mdp3 {

class PacketBuilder {
 public:
  explicit PacketBuilder(std::span<std::byte> buf) noexcept : buf_(buf) {}

  // Starts a new packet (drops anything written before). False if the header does not fit.
  bool begin(std::uint32_t msg_seq_num, std::uint64_t sending_time) noexcept;
  // Where the next message body goes (after MsgSize and the SBE header).
  [[nodiscard]] std::span<std::byte> body_space() noexcept;
  // Finalises a message whose body of `body_len` bytes was written into body_space().
  bool commit(const sbe::MessageHeader& header, std::size_t body_len) noexcept;
  template <class Writer>
  bool commit(const Writer& w) noexcept {
    return w.ok() && commit(Writer::header(), w.size_bytes());
  }

  [[nodiscard]] std::span<const std::byte> packet() const noexcept { return buf_.first(used_); }
  [[nodiscard]] std::size_t size() const noexcept { return used_; }
  [[nodiscard]] std::size_t messages() const noexcept { return messages_; }
  [[nodiscard]] std::size_t capacity() const noexcept { return buf_.size(); }

 private:
  std::span<std::byte> buf_;
  std::size_t used_ = 0;
  std::size_t messages_ = 0;
};

struct MbpEntry {
  std::int32_t security_id = 0;
  std::uint32_t rpt_seq = 0;
  schema::MDUpdateAction action = schema::MDUpdateAction::New;
  schema::MDEntryTypeBook type = schema::MDEntryTypeBook::Bid;
  std::uint8_t level = 1;  // MDPriceLevel, 1 = best
  Price price{};
  std::int32_t qty = 0;
  std::int32_t orders = 0;  // 0 encodes NumberOfOrders as null
};

struct TradeEntry {
  std::int32_t security_id = 0;
  std::uint32_t rpt_seq = 0;
  Price price{};
  std::int32_t qty = 0;
  std::int32_t orders = 0;
  schema::AggressorSide aggressor = schema::AggressorSide::NoAggressor;
  schema::MDUpdateAction action = schema::MDUpdateAction::New;
  std::uint32_t trade_entry_id = 0;  // 0 encodes MDTradeEntryID as null
};

struct SnapshotSpec {
  std::uint32_t last_msg_seq_num_processed = 0;
  std::uint32_t tot_num_reports = 0;
  std::int32_t security_id = 0;
  std::uint32_t rpt_seq = 0;
  std::uint64_t transact_time = 0;
};

struct DefinitionSpec {
  std::int32_t security_id = 0;
  std::string_view symbol;
  std::string_view security_group;
  std::string_view asset;
  Price tick{};
  std::int64_t display_factor_e9 = 1'000'000'000;  // DisplayFactor 1.0
  Qty unit_of_measure_qty{};                       // 0 encodes null
  std::int32_t contract_multiplier = 0;            // 0 encodes null
  std::uint8_t market_depth = kMaxMbpDepth;        // GBX
  std::uint8_t implied_depth = 0;                  // GBI, 0 = no implied feed type entry
  std::int16_t appl_id = 0;
  schema::SecurityUpdateAction update_action = schema::SecurityUpdateAction::Add;
  std::uint64_t last_update_time = 0;
};

bool encode_book46(PacketBuilder& p,
                   std::uint64_t transact_time,
                   schema::MatchEventIndicator mei,
                   std::span<const MbpEntry> entries) noexcept;
bool encode_trade48(PacketBuilder& p,
                    std::uint64_t transact_time,
                    schema::MatchEventIndicator mei,
                    std::span<const TradeEntry> entries) noexcept;
// Bids and asks best first; MDPriceLevel is the position.
bool encode_snapshot52(PacketBuilder& p,
                       const SnapshotSpec& spec,
                       std::span<const Level> bids,
                       std::span<const Level> asks) noexcept;
bool encode_definition54(PacketBuilder& p, const DefinitionSpec& spec) noexcept;
bool encode_channel_reset4(PacketBuilder& p,
                           std::uint64_t transact_time,
                           std::int16_t appl_id) noexcept;
bool encode_security_status30(PacketBuilder& p,
                              std::uint64_t transact_time,
                              std::int32_t security_id,
                              schema::SecurityTradingStatus status) noexcept;
bool encode_heartbeat12(PacketBuilder& p) noexcept;

// Encoded size of a Book46 / Trade48 message with n entries (MsgSize, header, root, groups).
[[nodiscard]] constexpr std::size_t book46_message_size(std::size_t n) noexcept {
  return kMessagePrefixSize + schema::MDIncrementalRefreshBook46::kBlockLength + 3 + 32 * n + 8;
}
[[nodiscard]] constexpr std::size_t trade48_message_size(std::size_t n) noexcept {
  return kMessagePrefixSize + schema::MDIncrementalRefreshTradeSummary48::kBlockLength + 3 +
         32 * n + 8;
}

class MbpPublisher {
 public:
  // Most entries one update() can produce for a side.
  static constexpr std::size_t kMaxEntriesPerUpdate = 2 * std::size_t{kMaxMbpDepth};

  MbpPublisher(std::int32_t security_id, std::uint8_t depth) noexcept;

  // Appends to `out` the entries that turn the published side into `target` (best first; only
  // the first depth() levels are published) and applies them. `out` must hold at least
  // kMaxEntriesPerUpdate entries; returns the number written. Quantities must be whole contracts.
  [[nodiscard]] std::size_t update(Side side,
                                   std::span<const Level> target,
                                   std::span<MbpEntry> out) noexcept;
  // RptSeq for a non-book entry (trade, statistic) of this instrument.
  [[nodiscard]] std::uint32_t next_rpt_seq() noexcept { return ++rpt_seq_; }
  [[nodiscard]] std::uint32_t rpt_seq() const noexcept { return rpt_seq_; }
  [[nodiscard]] std::span<const Level> levels(Side side) const noexcept {
    const auto s = static_cast<std::size_t>(side);
    return {levels_[s].data(), count_[s]};
  }
  [[nodiscard]] std::int32_t security_id() const noexcept { return security_id_; }
  [[nodiscard]] std::uint8_t depth() const noexcept { return depth_; }
  // Channel reset: empty book, RptSeq restarts at 1.
  void reset() noexcept;

 private:
  std::int32_t security_id_;
  std::uint8_t depth_;
  std::uint32_t rpt_seq_ = 0;
  std::array<std::array<Level, kMaxMbpDepth>, 2> levels_{};
  std::array<std::size_t, 2> count_{};
};

}  // namespace fastmm::codecs::mdp3
