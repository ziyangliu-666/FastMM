#pragma once
// Control-path decoders for Gate APIv4 futures REST bodies (allocation allowed). Endpoints and
// models from https://www.gate.com/docs/developers/apiv4/en/ (futures section) and the generated
// model docs of gateapi-python (Contract, FuturesAccount, Position, FuturesOrder, MyFuturesTrade),
// read 2026-10-03:
//   GET /api/v4/futures/{settle}/contracts            [] {name, type direct|inverse,
//       quanto_multiplier, order_price_round, order_size_min, order_size_max, funding_interval (s),
//       funding_next_apply, maker_fee_rate, taker_fee_rate, status, in_delisting, ...}
//   GET /api/v4/futures/{settle}/order_book?contract=C&limit=1  {current (s, fractional), ...}:
//       the venue's clock (fx-api.gateio.ws serves no time endpoint)
//   GET /api/v4/futures/{settle}/accounts             {user, total, available, order_margin,
//       position_margin, unrealised_pnl, maintenance_margin, currency, in_dual_mode, ...}
//   GET /api/v4/futures/{settle}/positions            [] {contract, size (signed), entry_price,
//       mode single|dual_long|dual_short, ...}
//   GET /api/v4/futures/{settle}/orders?status=open   [] FuturesOrder {id, contract, size (signed),
//       left (signed), price, status, text, tif, fill_price, create_time, finish_as, ...}
//   GET /api/v4/futures/{settle}/my_trades_timerange  [] {id, create_time (s, fractional),
//       contract, order_id, size (signed), price, role maker|taker, text, fee, point_fee}
//   GET /api/v4/futures/{settle}/fee                  {"<contract>": {taker_fee, maker_fee}}
// Every failure body is {"label":..,"message":..} (decode_error).
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/venues/balances.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::gate {

struct ContractInfo {
  std::string name;
  std::string type;  // "direct" (linear) | "inverse"
  std::string status;
  Qty quanto_multiplier{};  // base units per contract
  Price tick{};             // order_price_round
  Qty min_size{};           // order_size_min, contracts
  Qty max_size{};           // order_size_max, contracts
  std::int64_t funding_interval_s = 0;
  std::int64_t funding_next_apply_s = 0;
  double maker_fee_rate = 0.0;  // decimal (0.0002 = 2 bps); negative is a rebate
  double taker_fee_rate = 0.0;
  bool in_delisting = false;
};

struct AccountInfo {
  std::string user;  // the account's id, as the private channels name it
  std::string currency;
  BalanceFields fields{};  // free = available, locked = order + position margin, total, equity
  bool in_dual_mode = false;
};

struct PositionRecord {
  std::string contract;
  std::string mode;  // single | dual_long | dual_short
  Qty size{};        // signed contracts
  Price entry_price{};
};

struct OpenOrderRecord {
  std::string contract;
  std::string id;    // the venue's order id (decimal text)
  std::string text;  // our "t-<client id>" or the venue's source tag
  std::string status;
  Qty size{};  // signed: positive bid
  Qty left{};  // signed, unfilled
  Price price{};
  std::int64_t create_time_ms = 0;
};

struct TradeRecord {
  std::string id;
  std::string contract;
  std::string order_id;
  std::string text;
  Qty size{};  // signed contracts
  Price price{};
  Notional fee{};  // settle currency, positive paid
  bool maker = false;
  std::int64_t time_ms = 0;
};

struct FeeRates {
  double maker = 0.0;
  double taker = 0.0;
};

// Empty string on success, else an error description.
std::string decode_contracts(std::string_view json, std::vector<ContractInfo>& out);
std::string decode_contract(std::string_view json, ContractInfo& out);
// `current` of an order-book reply (or server_time of /spot/time) as Unix ms.
std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms);
std::string decode_account(std::string_view json, AccountInfo& out);
std::string decode_positions(std::string_view json, std::vector<PositionRecord>& out);
std::string decode_open_orders(std::string_view json, std::vector<OpenOrderRecord>& out);
std::string decode_my_trades(std::string_view json, std::vector<TradeRecord>& out);
// {"BTC_USDT":{"taker_fee":"0.0005","maker_fee":"0.0002"},...}
std::string decode_fees(std::string_view json, std::vector<std::pair<std::string, FeeRates>>& out);
// {"label":..,"message":..}; false if the body is not an error object.
bool decode_error(std::string_view json, std::string& label, std::string& message);

// A signed Gate size (JSON number or string, possibly fractional) as |size| and its sign.
// False if unreadable.
bool parse_signed_size(std::string_view text, Qty& magnitude, bool& positive) noexcept;

}  // namespace fastmm::venues::gate
