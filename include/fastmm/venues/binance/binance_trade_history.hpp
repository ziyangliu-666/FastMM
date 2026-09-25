#pragma once
// One execution from Binance's trade history (Spot GET /api/v3/myTrades, USDⓈ-M
// GET /fapi/v1/userTrades) as a replayed fill: the trade id is the execution id, and the
// commission is an amount of commissionAsset - quote units are a Notional, base units a Qty the
// engine converts at the fill price, anything else (BNB) it cannot value.
#include "fastmm/core/instrument.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/connector_common.hpp"
#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/order_events.hpp"
#include "fastmm/venues/symbology.hpp"

namespace fastmm::venues::binance {

// False when the trade's price or quantity does not parse (nothing is emitted).
inline bool emit_trade_history_fill(EventSink& sink,
                                    VenueId venue,
                                    const Instrument& inst,
                                    InstrumentId id,
                                    ClientOrderId cl_ord_id,
                                    const MyTradeRecord& t) noexcept {
  const auto px = parse_price(t.price);
  const auto qty = parse_qty(t.qty);
  if (!px || !qty) return false;
  Notional fee{};
  FeeAsset fee_asset = FeeAsset::Quote;
  if (const auto f = parse_qty(t.commission)) fee = Notional::from_raw(f->raw);
  if (!fee.is_zero() && !t.commission_asset.empty()) {
    if (iequals_symbol(inst.quote.view(), t.commission_asset)) {
      fee_asset = FeeAsset::Quote;
    } else if (iequals_symbol(inst.base.view(), t.commission_asset)) {
      fee_asset = FeeAsset::Base;
    } else {
      fee_asset = FeeAsset::Other;
    }
  }
  emit_replayed_fill(sink,
                     venue,
                     id,
                     cl_ord_id,
                     IdText(t.order_id).view(),
                     IdText(t.id).view(),
                     t.is_buyer ? Side::Buy : Side::Sell,
                     *px,
                     *qty,
                     fee,
                     fee_asset,
                     t.is_maker ? Liquidity::Maker : Liquidity::Taker);
  return true;
}

}  // namespace fastmm::venues::binance
