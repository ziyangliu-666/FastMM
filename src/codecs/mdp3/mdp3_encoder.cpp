#include "fastmm/codecs/mdp3/mdp3_encoder.hpp"

#include <algorithm>

namespace fastmm::codecs::mdp3 {

namespace {

using Book46 = schema::MDIncrementalRefreshBook46;
using Trade48 = schema::MDIncrementalRefreshTradeSummary48;
using Snap52 = schema::SnapshotFullRefresh52;
using Def54 = schema::MDInstrumentDefinitionFuture54;

[[nodiscard]] constexpr bool better(Side side, Price a, Price b) noexcept {
  return side == Side::Buy ? a > b : a < b;
}

[[nodiscard]] std::int32_t whole_contracts(Qty q) noexcept {
  return static_cast<std::int32_t>(q.raw / kFixedScale);
}

}  // namespace

// ---- PacketBuilder ------------------------------------------------------------------------------

bool PacketBuilder::begin(std::uint32_t msg_seq_num, std::uint64_t sending_time) noexcept {
  used_ = 0;
  messages_ = 0;
  if (buf_.size() < kPacketHeaderSize) return false;
  PacketHeader{msg_seq_num, sending_time}.store(buf_.data());
  used_ = kPacketHeaderSize;
  return true;
}

std::span<std::byte> PacketBuilder::body_space() noexcept {
  const std::size_t at = std::min(used_ + kMessagePrefixSize, buf_.size());
  return buf_.subspan(at);
}

bool PacketBuilder::commit(const sbe::MessageHeader& header, std::size_t body_len) noexcept {
  const std::size_t total = kMessagePrefixSize + body_len;
  if (used_ < kPacketHeaderSize || used_ + total > buf_.size() || total > 0xFFFF) return false;
  sbe::store_le(buf_.data() + used_, static_cast<std::uint16_t>(total));
  header.store(buf_.data() + used_ + kMsgSizeFieldSize);
  used_ += total;
  ++messages_;
  return true;
}

// ---- messages -----------------------------------------------------------------------------------

bool encode_book46(PacketBuilder& p,
                   std::uint64_t transact_time,
                   schema::MatchEventIndicator mei,
                   std::span<const MbpEntry> entries) noexcept {
  schema::MDIncrementalRefreshBook46Writer w(p.body_space());
  w.set_transact_time(transact_time);
  w.set_match_event_indicator(mei);
  const auto g = w.no_md_entries(entries.size());
  if (!g.ok()) return false;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const MbpEntry& e = entries[i];
    schema::PRICENULL9 px{};
    if (!schema::PRICENULL9::from_fixed(e.price, px)) return false;
    auto ew = g[i];
    ew.set_md_entry_px(px);
    ew.set_md_entry_size(e.qty);
    ew.set_security_id(e.security_id);
    ew.set_rpt_seq(e.rpt_seq);
    ew.set_number_of_orders(e.orders > 0 ? e.orders : Book46::NoMDEntries::kNumberOfOrdersNull);
    ew.set_md_price_level(e.level);
    ew.set_md_update_action(e.action);
    ew.set_md_entry_type(e.type);
    ew.set_tradeable_size(Book46::NoMDEntries::kTradeableSizeNull);
  }
  static_cast<void>(w.no_order_id_entries(0));
  return p.commit(w);
}

bool encode_trade48(PacketBuilder& p,
                    std::uint64_t transact_time,
                    schema::MatchEventIndicator mei,
                    std::span<const TradeEntry> entries) noexcept {
  schema::MDIncrementalRefreshTradeSummary48Writer w(p.body_space());
  w.set_transact_time(transact_time);
  w.set_match_event_indicator(mei);
  const auto g = w.no_md_entries(entries.size());
  if (!g.ok()) return false;
  for (std::size_t i = 0; i < entries.size(); ++i) {
    const TradeEntry& e = entries[i];
    schema::PRICE9 px{};
    if (!schema::PRICE9::from_fixed(e.price, px)) return false;
    auto ew = g[i];
    ew.set_md_entry_px(px);
    ew.set_md_entry_size(e.qty);
    ew.set_security_id(e.security_id);
    ew.set_rpt_seq(e.rpt_seq);
    ew.set_number_of_orders(e.orders);
    ew.set_aggressor_side(e.aggressor);
    ew.set_md_update_action(e.action);
    ew.set_md_trade_entry_id(e.trade_entry_id != 0 ? e.trade_entry_id
                                                   : Trade48::NoMDEntries::kMDTradeEntryIDNull);
  }
  static_cast<void>(w.no_order_id_entries(0));
  return p.commit(w);
}

bool encode_snapshot52(PacketBuilder& p,
                       const SnapshotSpec& spec,
                       std::span<const Level> bids,
                       std::span<const Level> asks) noexcept {
  if (bids.size() > kMaxMbpDepth || asks.size() > kMaxMbpDepth) return false;
  schema::SnapshotFullRefresh52Writer w(p.body_space());
  w.set_last_msg_seq_num_processed(spec.last_msg_seq_num_processed);
  w.set_tot_num_reports(spec.tot_num_reports);
  w.set_security_id(spec.security_id);
  w.set_rpt_seq(spec.rpt_seq);
  w.set_transact_time(spec.transact_time);
  w.set_last_update_time(0);
  w.set_trade_date(Snap52::kTradeDateNull);
  w.set_md_security_trading_status(schema::SecurityTradingStatus::ReadyToTrade);
  w.set_high_limit_price(schema::PRICENULL9::null());
  w.set_low_limit_price(schema::PRICENULL9::null());
  w.set_max_price_variation(schema::PRICENULL9::null());
  const auto g = w.no_md_entries(bids.size() + asks.size());
  if (!g.ok()) return false;
  std::size_t n = 0;
  for (std::size_t side = 0; side < 2; ++side) {
    const std::span<const Level> levels = side == 0 ? bids : asks;
    for (std::size_t k = 0; k < levels.size(); ++k) {
      schema::PRICENULL9 px{};
      if (!schema::PRICENULL9::from_fixed(levels[k].price, px)) return false;
      auto ew = g[n++];
      ew.set_md_entry_px(px);
      ew.set_md_entry_size(whole_contracts(levels[k].qty));
      ew.set_number_of_orders(Snap52::NoMDEntries::kNumberOfOrdersNull);
      ew.set_md_price_level(static_cast<std::int8_t>(k + 1));
      ew.set_trading_reference_date(Snap52::NoMDEntries::kTradingReferenceDateNull);
      ew.set_open_close_settl_flag(schema::OpenCloseSettlFlag::NullValue);
      ew.set_md_entry_type(side == 0 ? schema::MDEntryType::Bid : schema::MDEntryType::Offer);
    }
  }
  return p.commit(w);
}

bool encode_definition54(PacketBuilder& p, const DefinitionSpec& spec) noexcept {
  schema::PRICE9 tick{};
  if (!schema::PRICE9::from_fixed(spec.tick, tick)) return false;
  schema::Decimal9NULL uom = schema::Decimal9NULL::null();
  if (spec.unit_of_measure_qty.is_positive() &&
      !schema::Decimal9NULL::from_fixed(spec.unit_of_measure_qty, uom)) {
    return false;
  }
  schema::MDInstrumentDefinitionFuture54Writer w(p.body_space());
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  w.set_match_event_indicator(mei);
  w.set_tot_num_reports(Def54::kTotNumReportsNull);
  w.set_security_update_action(spec.update_action);
  w.set_last_update_time(spec.last_update_time);
  w.set_md_security_trading_status(schema::SecurityTradingStatus::ReadyToTrade);
  w.set_appl_id(spec.appl_id);
  w.set_market_segment_id(0);
  w.set_underlying_product(0);
  w.set_security_exchange("XCME");
  w.set_security_group(spec.security_group);
  w.set_asset(spec.asset);
  w.set_symbol(spec.symbol);
  w.set_security_id(spec.security_id);
  w.set_security_type("FUT");
  w.set_cfi_code("FXXXXX");
  w.set_maturity_month_year(schema::MaturityMonthYear{schema::MaturityMonthYear::kYearNull,
                                                      schema::MaturityMonthYear::kMonthNull,
                                                      schema::MaturityMonthYear::kDayNull,
                                                      schema::MaturityMonthYear::kWeekNull});
  w.set_currency("USD");
  w.set_settl_currency("USD");
  w.set_match_algorithm('F');
  w.set_min_trade_vol(1);
  w.set_max_trade_vol(10'000);
  w.set_min_price_increment(tick);
  w.set_display_factor(schema::Decimal9{spec.display_factor_e9});
  w.set_main_fraction(Def54::kMainFractionNull);
  w.set_sub_fraction(Def54::kSubFractionNull);
  w.set_price_display_format(Def54::kPriceDisplayFormatNull);
  w.set_unit_of_measure("");
  w.set_unit_of_measure_qty(uom);
  w.set_trading_reference_price(schema::PRICENULL9::null());
  w.set_open_interest_qty(Def54::kOpenInterestQtyNull);
  w.set_cleared_volume(Def54::kClearedVolumeNull);
  w.set_high_limit_price(schema::PRICENULL9::null());
  w.set_low_limit_price(schema::PRICENULL9::null());
  w.set_max_price_variation(schema::PRICENULL9::null());
  w.set_decay_quantity(Def54::kDecayQuantityNull);
  w.set_decay_start_date(Def54::kDecayStartDateNull);
  w.set_original_contract_size(Def54::kOriginalContractSizeNull);
  w.set_contract_multiplier(spec.contract_multiplier != 0 ? spec.contract_multiplier
                                                          : Def54::kContractMultiplierNull);
  w.set_contract_multiplier_unit(Def54::kContractMultiplierUnitNull);
  w.set_flow_schedule_type(Def54::kFlowScheduleTypeNull);
  w.set_min_price_increment_amount(schema::PRICENULL9::null());
  w.set_user_defined_instrument('N');
  w.set_trading_reference_date(Def54::kTradingReferenceDateNull);
  w.set_instrument_guid(Def54::kInstrumentGUIDNull);
  static_cast<void>(w.no_events(0));
  const std::size_t feeds = spec.implied_depth != 0 ? 2 : 1;
  const auto g = w.no_md_feed_types(feeds);
  if (!g.ok()) return false;
  g[0].set_md_feed_type("GBX");
  g[0].set_market_depth(static_cast<std::int8_t>(spec.market_depth));
  if (feeds == 2) {
    g[1].set_md_feed_type("GBI");
    g[1].set_market_depth(static_cast<std::int8_t>(spec.implied_depth));
  }
  static_cast<void>(w.no_inst_attrib(0));
  static_cast<void>(w.no_lot_type_rules(0));
  return p.commit(w);
}

bool encode_channel_reset4(PacketBuilder& p,
                           std::uint64_t transact_time,
                           std::int16_t appl_id) noexcept {
  schema::ChannelReset4Writer w(p.body_space());
  w.set_transact_time(transact_time);
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  w.set_match_event_indicator(mei);
  const auto g = w.no_md_entries(1);
  if (!g.ok()) return false;
  g[0].set_appl_id(appl_id);
  return p.commit(w);
}

bool encode_security_status30(PacketBuilder& p,
                              std::uint64_t transact_time,
                              std::int32_t security_id,
                              schema::SecurityTradingStatus status) noexcept {
  schema::SecurityStatus30Writer w(p.body_space());
  w.set_transact_time(transact_time);
  w.set_security_group("");
  w.set_asset("");
  w.set_security_id(security_id);
  w.set_trade_date(schema::SecurityStatus30::kTradeDateNull);
  schema::MatchEventIndicator mei{};
  mei.set_end_of_event(true);
  w.set_match_event_indicator(mei);
  w.set_security_trading_status(status);
  w.set_halt_reason(schema::HaltReason::GroupSchedule);
  w.set_security_trading_event(schema::SecurityTradingEvent::NoEvent);
  return p.commit(w);
}

bool encode_heartbeat12(PacketBuilder& p) noexcept {
  schema::AdminHeartbeat12Writer w(p.body_space());
  return p.commit(w);
}

// ---- MbpPublisher -------------------------------------------------------------------------------

MbpPublisher::MbpPublisher(std::int32_t security_id, std::uint8_t depth) noexcept
    : security_id_(security_id), depth_(std::clamp<std::uint8_t>(depth, 1, kMaxMbpDepth)) {}

void MbpPublisher::reset() noexcept {
  count_ = {};
  rpt_seq_ = 0;
}

// Walks both lists best-first. At position k:
//   published level better than the target (or no target): it is gone     -> Delete k
//   target better than the published level (or none left): a new level    -> New k
//   same price: quantity may differ                                        -> Change k
// Delete shifts worse published levels up and New shifts them down (dropping the one past
// depth), exactly as the receiver applies them, so after the walk the published side equals the
// first depth() target levels.
std::size_t MbpPublisher::update(Side side,
                                 std::span<const Level> target,
                                 std::span<MbpEntry> out) noexcept {
  if (out.size() < kMaxEntriesPerUpdate) return 0;
  const auto s = static_cast<std::size_t>(side);
  std::array<Level, kMaxMbpDepth>& w = levels_[s];
  std::size_t& n = count_[s];
  const std::size_t tn = std::min<std::size_t>(target.size(), depth_);
  const auto type =
      side == Side::Buy ? schema::MDEntryTypeBook::Bid : schema::MDEntryTypeBook::Offer;
  std::size_t written = 0;
  const auto emit = [&](schema::MDUpdateAction action, std::size_t k, const Level& l) noexcept {
    MbpEntry& e = out[written++];
    e.security_id = security_id_;
    e.rpt_seq = ++rpt_seq_;
    e.action = action;
    e.type = type;
    e.level = static_cast<std::uint8_t>(k + 1);
    e.price = l.price;
    e.qty = whole_contracts(l.qty);
    e.orders = 0;
  };
  std::size_t k = 0;
  while (k < depth_ && (k < n || k < tn)) {
    if (k < n && (k >= tn || better(side, w[k].price, target[k].price))) {
      emit(schema::MDUpdateAction::Delete, k, w[k]);
      for (std::size_t j = k; j + 1 < n; ++j) w[j] = w[j + 1];
      --n;
      continue;
    }
    if (k >= n || better(side, target[k].price, w[k].price)) {
      emit(schema::MDUpdateAction::New, k, target[k]);
      if (n == depth_) --n;
      for (std::size_t j = n; j > k; --j) w[j] = w[j - 1];
      w[k] = target[k];
      ++n;
      ++k;
      continue;
    }
    if (w[k].qty != target[k].qty) {
      emit(schema::MDUpdateAction::Change, k, target[k]);
      w[k].qty = target[k].qty;
    }
    ++k;
  }
  return written;
}

}  // namespace fastmm::codecs::mdp3
