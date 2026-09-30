#include "fastmm/venues/coinbase/advanced_user_parser.hpp"

#include "fastmm/venues/coinbase/advanced_error_map.hpp"
#include "fastmm/venues/coinbase/coinbase_wire.hpp"
#include "fastmm/venues/decimal.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <cstring>

namespace fastmm::venues::coinbase {

namespace sj = simdjson;
namespace od = simdjson::ondemand;

struct AdvancedUserParser::Impl {
  od::parser parser;
  explicit Impl(std::size_t capacity) {
    if (parser.allocate(capacity) != sj::SUCCESS) std::abort();
  }
};

AdvancedUserParser::AdvancedUserParser(const SymbolTable& symbols,
                                       VenueId venue,
                                       std::size_t capacity)
    : impl_(std::make_unique<Impl>(capacity)), symbols_(symbols), venue_(venue) {}
AdvancedUserParser::~AdvancedUserParser() = default;

namespace {

[[nodiscard]] inline sj::padded_string_view padded(std::string_view s) noexcept {
  return sj::padded_string_view(s.data(), s.size(), s.size() + sj::SIMDJSON_PADDING);
}

struct OrderFields {
  std::string_view order_id;
  std::string_view client_order_id;
  std::string_view cumulative_quantity;
  std::string_view status;
  std::string_view product_id;
  std::string_view time_in_force;
  std::string_view reject_reason;
};

[[gnu::noinline]] bool read_order(od::object& o, OrderFields& f) noexcept {
  for (auto field : o) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return false;
    std::string_view* dst = key == "order_id"              ? &f.order_id
                            : key == "client_order_id"     ? &f.client_order_id
                            : key == "cumulative_quantity" ? &f.cumulative_quantity
                            : key == "status"              ? &f.status
                            : key == "product_id"          ? &f.product_id
                            : key == "time_in_force"       ? &f.time_in_force
                            : key == "reject_reason"       ? &f.reject_reason
                                                           : nullptr;
    if (dst == nullptr) continue;
    if (field.value().get_string().get(*dst) != sj::SUCCESS) *dst = {};
  }
  return true;
}

template <class M>
M* place(std::span<std::byte> out, std::uint32_t written) noexcept {
  std::byte* p = out.data() + written;
  std::memset(p, 0, sizeof(M));
  return reinterpret_cast<M*>(p);
}

}  // namespace

void AdvancedUserParser::decode(std::string_view json,
                                Timestamp recv_ts,
                                Cycles t0,
                                std::span<std::byte> out,
                                AdvancedUserResult& r) noexcept {
  ++stats_.frames;
  r.status = ParseStatus::Ignored;
  r.count = 0;
  r.len = 0;
  r.due_count = 0;
  r.control = UserControl::None;
  r.msg = {};
  r.has_sequence = false;
  auto bad = [&] {
    ++stats_.malformed;
    r.status = ParseStatus::Malformed;
    r.count = 0;
    r.len = 0;
    r.due_count = 0;
  };
  if (out.size() < kDecoderScratchBytes) {
    r.status = ParseStatus::Overflow;
    return;
  }
  od::document doc;
  od::object root;
  if (impl_->parser.iterate(padded(json)).get(doc) != sj::SUCCESS ||
      doc.get_object().get(root) != sj::SUCCESS)
    return bad();
  std::string_view channel;
  std::uint32_t written = 0;
  auto stamp = [&](EventHeader& h) {
    h.recv_ts = recv_ts;
    h.t0_cycles = t0;
  };
  auto order = [&](const OrderFields& f) -> bool {
    ++stats_.orders;
    const auto cl = decode_cl_ord_id(f.client_order_id);
    if (!cl) {
      ++stats_.foreign;
      return true;
    }
    const InstrumentId inst = symbols_.find(venue_, f.product_id);
    if (!inst.valid()) {
      ++stats_.unknown_symbol;
      return true;
    }
    if (f.status.empty() || f.order_id.empty()) return false;
    Seen* seen = orders_.find(*cl);
    if (seen == nullptr) {
      seen = orders_.insert(*cl, Seen{}).first;
      if (seen == nullptr) {
        ++stats_.table_full;
        return true;
      }
    }
    const bool failed = f.status == "FAILED";
    if (!seen->acked && !failed) {
      if (written + sizeof(OrderAckMsg) > out.size()) return true;
      auto* m = place<OrderAckMsg>(out, written);
      init_header(*m, EventType::OrderAck, inst, venue_);
      m->cl_ord_id = *cl;
      m->venue_order_id.assign(f.order_id);
      stamp(m->hdr);
      written += sizeof(OrderAckMsg);
      ++r.count;
      seen->acked = true;
    }
    Qty cum{};
    if (!f.cumulative_quantity.empty()) {
      const auto c = parse_qty(f.cumulative_quantity);
      if (!c) return false;
      cum = *c;
    }
    if (cum > seen->cum) {
      seen->cum = cum;
      if (r.due_count < AdvancedUserResult::kMaxDue) {
        FillDue& d = r.due[r.due_count++];
        d.cl = *cl;
        d.instrument = inst;
        d.cum = cum;
        d.order_id.assign(f.order_id);
        ++stats_.fills_due;
      }
    }
    const bool ioc = f.time_in_force == "IMMEDIATE_OR_CANCEL" || f.time_in_force == "FILL_OR_KILL";
    if (f.status == "CANCELLED" || f.status == "EXPIRED") {
      if (written + sizeof(OrderCancelAckMsg) > out.size()) return true;
      if (f.status == "EXPIRED" || ioc) {
        auto* m = place<OrderExpiredMsg>(out, written);
        init_header(*m, EventType::OrderExpired, inst, venue_);
        m->cl_ord_id = *cl;
        m->venue_order_id.assign(f.order_id);
        m->cum_qty = cum;
        stamp(m->hdr);
        written += sizeof(OrderExpiredMsg);
      } else {
        auto* m = place<OrderCancelAckMsg>(out, written);
        init_header(*m, EventType::OrderCancelAck, inst, venue_);
        m->cl_ord_id = *cl;
        m->venue_order_id.assign(f.order_id);
        m->cum_qty = cum;
        stamp(m->hdr);
        written += sizeof(OrderCancelAckMsg);
      }
      ++r.count;
      orders_.erase(*cl);
    } else if (failed) {
      if (written + sizeof(OrderRejectMsg) > out.size()) return true;
      auto* m = place<OrderRejectMsg>(out, written);
      init_header(*m, EventType::OrderReject, inst, venue_);
      m->cl_ord_id = *cl;
      m->reason = map_order_failure(f.reject_reason).reason;
      m->text.assign(f.reject_reason);
      stamp(m->hdr);
      written += sizeof(OrderRejectMsg);
      ++r.count;
      orders_.erase(*cl);
    } else if (f.status == "FILLED") {
      orders_.erase(*cl);
    }
    return true;
  };

  for (auto field : root) {
    std::string_view key;
    if (field.unescaped_key().get(key) != sj::SUCCESS) return bad();
    if (key == "channel") {
      if (field.value().get_string().get(channel) != sj::SUCCESS) return bad();
    } else if (key == "sequence_num") {
      if (field.value().get_uint64().get(r.sequence) != sj::SUCCESS) return bad();
      r.has_sequence = true;
    } else if (key == "type") {
      std::string_view type;
      if (field.value().get_string().get(type) == sj::SUCCESS && type == "error")
        r.control = UserControl::Error;
    } else if (key == "message") {
      if (field.value().get_string().get(r.msg) != sj::SUCCESS) r.msg = {};
    } else if (key == "events" && channel == "user") {
      od::array events;
      if (field.value().get_array().get(events) != sj::SUCCESS) return bad();
      for (auto ev_res : events) {
        od::object ev;
        if (ev_res.get_object().get(ev) != sj::SUCCESS) return bad();
        for (auto ef : ev) {
          std::string_view ek;
          if (ef.unescaped_key().get(ek) != sj::SUCCESS) return bad();
          if (ek != "orders") continue;
          od::array orders;
          if (ef.value().get_array().get(orders) != sj::SUCCESS) return bad();
          for (auto o_res : orders) {
            od::object o;
            OrderFields f;
            if (o_res.get_object().get(o) != sj::SUCCESS || !read_order(o, f) || !order(f))
              return bad();
          }
        }
      }
    }
  }
  if (r.control == UserControl::Error) {
    ++stats_.control;
    r.status = ParseStatus::Error;
    return;
  }
  if (channel == "subscriptions" || channel == "heartbeats") {
    ++stats_.control;
    r.control = channel == "heartbeats" ? UserControl::Heartbeat : UserControl::Subscriptions;
    return;
  }
  if (channel.empty()) return bad();
  if (r.count > 0) {
    r.status = ParseStatus::Ok;
    r.len = written;
  } else if (r.due_count == 0) {
    ++stats_.ignored;
  }
}

}  // namespace fastmm::venues::coinbase
