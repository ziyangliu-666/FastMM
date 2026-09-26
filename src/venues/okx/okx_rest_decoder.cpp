#include "fastmm/venues/okx/okx_rest_decoder.hpp"

#include "fastmm/venues/decimal.hpp"
#include "fastmm/venues/okx/okx_error_map.hpp"

#include <simdjson.h>

namespace fastmm::venues::okx {

namespace sj = simdjson;
namespace dom = simdjson::dom;

namespace {

// The data array of an envelope with code "0"; an error text otherwise.
std::string open_data(dom::parser& parser,
                      std::string_view json,
                      std::string_view what,
                      dom::array& data) {
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return std::string(what) + ": invalid JSON";
  std::string_view code;
  if (root["code"].get(code) != sj::SUCCESS) return std::string(what) + ": missing code";
  if (code != "0") {
    std::string_view msg;
    if (root["msg"].get(msg) != sj::SUCCESS) msg = {};
    return std::string(what) + ": code " + std::string(code) + " " + std::string(msg);
  }
  if (root["data"].get(data) != sj::SUCCESS) return std::string(what) + ": missing data";
  return {};
}

std::string text(const dom::element& e, const char* key) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS) return {};
  return std::string(s);
}

template <class F>
bool fixed(const dom::element& e, const char* key, F& out) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS || s.empty()) return false;
  const auto v = parse_fixed<F>(s);
  if (!v) return false;
  out = *v;
  return true;
}

bool millis(const dom::element& e, const char* key, std::int64_t& out) {
  std::string_view s;
  if (e[key].get(s) != sj::SUCCESS) return false;
  const auto v = parse_int64(s);
  if (!v) return false;
  out = *v;
  return true;
}

}  // namespace

std::string decode_instruments(std::string_view json, std::vector<InstrumentInfo>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "instruments", data); !err.empty()) return err;
  for (dom::element e : data) {
    InstrumentInfo i;
    i.inst_id = text(e, "instId");
    if (i.inst_id.empty()) return "instruments: entry without instId";
    std::int64_t code = -1;
    if (e["instIdCode"].get(code) == sj::SUCCESS) i.inst_id_code = code;
    i.inst_type = text(e, "instType");
    i.ct_type = text(e, "ctType");
    i.ct_val_ccy = text(e, "ctValCcy");
    i.settle_ccy = text(e, "settleCcy");
    i.state = text(e, "state");
    if (!fixed(e, "tickSz", i.tick)) return "instruments: bad tickSz for " + i.inst_id;
    if (!fixed(e, "lotSz", i.lot)) return "instruments: bad lotSz for " + i.inst_id;
    if (!fixed(e, "ctVal", i.ct_val)) return "instruments: bad ctVal for " + i.inst_id;
    if (!fixed(e, "ctMult", i.ct_mult)) i.ct_mult = Qty::from_int(1);
    static_cast<void>(fixed(e, "minSz", i.min_sz));
    static_cast<void>(fixed(e, "maxLmtSz", i.max_limit_sz));
    out.push_back(std::move(i));
  }
  return {};
}

std::string decode_server_time(std::string_view json, std::int64_t& server_time_ms) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "time", data); !err.empty()) return err;
  for (dom::element e : data) {
    if (millis(e, "ts", server_time_ms)) return {};
  }
  return "time: missing ts";
}

std::string decode_account_config(std::string_view json, AccountConfig& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "account config", data); !err.empty()) return err;
  for (dom::element e : data) {
    out.pos_mode = text(e, "posMode");
    out.acct_lv = text(e, "acctLv");
    if (out.pos_mode.empty()) return "account config: missing posMode";
    return {};
  }
  return "account config: empty data";
}

std::string decode_positions(std::string_view json, std::vector<PositionRecord>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "positions", data); !err.empty()) return err;
  for (dom::element e : data) {
    PositionRecord p;
    p.inst_id = text(e, "instId");
    p.pos_side = text(e, "posSide");
    if (p.inst_id.empty()) return "positions: entry without instId";
    std::string_view pos;
    if (e["pos"].get(pos) == sj::SUCCESS && !pos.empty()) {
      const auto q = parse_qty(pos);
      if (!q) return "positions: bad pos for " + p.inst_id;
      p.qty = *q;
    }
    static_cast<void>(fixed(e, "avgPx", p.avg_px));
    out.push_back(std::move(p));
  }
  return {};
}

std::string decode_pending_orders(std::string_view json, std::vector<PendingOrder>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "orders-pending", data); !err.empty()) return err;
  for (dom::element e : data) {
    PendingOrder o;
    o.inst_id = text(e, "instId");
    o.ord_id = text(e, "ordId");
    o.cl_ord_id = text(e, "clOrdId");
    o.side = text(e, "side");
    o.state = text(e, "state");
    if (o.inst_id.empty() || o.ord_id.empty()) return "orders-pending: entry without ids";
    // A market order has no price; nothing FastMM rests is one.
    static_cast<void>(fixed(e, "px", o.px));
    if (!fixed(e, "sz", o.sz)) return "orders-pending: bad sz for " + o.ord_id;
    static_cast<void>(fixed(e, "accFillSz", o.acc_fill_sz));
    out.push_back(std::move(o));
  }
  return {};
}

std::string decode_fills(std::string_view json, std::vector<FillRecord>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "fills", data); !err.empty()) return err;
  for (dom::element e : data) {
    FillRecord f;
    f.inst_id = text(e, "instId");
    f.trade_id = text(e, "tradeId");
    f.ord_id = text(e, "ordId");
    f.cl_ord_id = text(e, "clOrdId");
    f.bill_id = text(e, "billId");
    f.side = text(e, "side");
    f.exec_type = text(e, "execType");
    f.fee_ccy = text(e, "feeCcy");
    if (f.inst_id.empty() || f.trade_id.empty() || f.bill_id.empty())
      return "fills: entry without instId, tradeId or billId";
    if (!fixed(e, "fillPx", f.px) || !fixed(e, "fillSz", f.sz))
      return "fills: bad fillPx or fillSz for trade " + f.trade_id;
    static_cast<void>(fixed(e, "fee", f.fee));
    if (!millis(e, "ts", f.ts_ms)) return "fills: bad ts for trade " + f.trade_id;
    if (!millis(e, "fillTime", f.fill_time_ms)) f.fill_time_ms = f.ts_ms;
    out.push_back(std::move(f));
  }
  return {};
}

std::string decode_bills(std::string_view json, std::vector<BillRecord>& out) {
  dom::parser parser;
  dom::array data;
  if (std::string err = open_data(parser, json, "bills", data); !err.empty()) return err;
  for (dom::element e : data) {
    BillRecord b;
    b.bill_id = text(e, "billId");
    b.inst_id = text(e, "instId");
    b.ccy = text(e, "ccy");
    b.type = text(e, "type");
    b.sub_type = text(e, "subType");
    if (b.bill_id.empty()) return "bills: entry without billId";
    if (!fixed(e, "balChg", b.bal_chg)) return "bills: bad balChg for bill " + b.bill_id;
    if (!millis(e, "ts", b.ts_ms)) return "bills: bad ts for bill " + b.bill_id;
    out.push_back(std::move(b));
  }
  return {};
}

std::string decode_cancel_batch(std::string_view json, std::vector<std::string>& failed) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS)
    return "cancel-batch-orders: invalid JSON";
  std::string_view code;
  if (root["code"].get(code) != sj::SUCCESS) return "cancel-batch-orders: missing code";
  dom::array data;
  if (root["data"].get(data) != sj::SUCCESS || data.size() == 0) {
    // A request-level failure: nothing was cancelled.
    if (code == "0") return {};
    std::string_view msg;
    if (root["msg"].get(msg) != sj::SUCCESS) msg = {};
    return "cancel-batch-orders: code " + std::string(code) + " " + std::string(msg);
  }
  for (dom::element e : data) {
    const std::string s_code = text(e, "sCode");
    const int c = parse_code(s_code);
    if (c == 0 || c == 51400) continue;  // cancelled, or already filled / cancelled / gone
    failed.push_back(text(e, "ordId") + ": " + s_code + " " + text(e, "sMsg"));
  }
  return {};
}

bool decode_envelope(std::string_view json, int& code, std::string& msg) {
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(json)).get(root) != sj::SUCCESS) return false;
  std::string_view c;
  if (root["code"].get(c) != sj::SUCCESS) return false;
  code = parse_code(c);
  std::string_view m;
  if (root["msg"].get(m) == sj::SUCCESS) msg = std::string(m);
  return true;
}

}  // namespace fastmm::venues::okx
