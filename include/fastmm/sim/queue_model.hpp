#pragma once
// QueuePositionModel (8.2): fills for passive orders against HISTORICAL L2 data, where no
// counter-party orders exist to match against.
//
//   place        ahead = displayed qty at our price (we join the back of the queue)
//   level shrink the reduction r at our price is split between cancels ahead of us and
//                behind us: ahead -= r * ahead / old_qty * (1 - conservatism)
//                (conservatism 1 => cancels never help us; 0 => proportional share)
//                a level that disappears entirely sets ahead = 0
//   trade at px  consumes `ahead` first, the surplus fills us: min(trade - ahead, leaves)
//   trade through (better than our price for the aggressor) fills us completely
//
// Orders live in a Pool; iteration is in handle order for determinism.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/containers/open_hash_map.hpp"
#include "fastmm/core/containers/pool.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>

namespace fastmm::sim {

inline constexpr std::size_t kMaxQueuedOrders = 4096;

struct QueuedOrder {
  ClientOrderId cl_ord_id;
  std::uint64_t order_id;
  Price price;
  Qty qty;
  Qty cum_qty;
  Qty ahead;  // displayed quantity still ahead of us in the queue
  InstrumentId instrument;
  Side side;
  std::uint8_t pad_[3];
  [[nodiscard]] constexpr Qty leaves() const noexcept { return qty - cum_qty; }
};
static_assert(sizeof(QueuedOrder) == 56 && std::is_trivially_copyable_v<QueuedOrder>);

class QueuePositionModel {
 public:
  using Handle32 = Handle<QueuedOrder>;

  // conservatism_bps in [0, 10000]; 10000 == cancels ahead never shorten the queue.
  explicit QueuePositionModel(std::int64_t conservatism_bps = 10'000) noexcept
      : conservatism_bps_(conservatism_bps < 0        ? 0
                          : conservatism_bps > 10'000 ? 10'000
                                                      : conservatism_bps) {}
  QueuePositionModel(const QueuePositionModel&) = delete;
  QueuePositionModel& operator=(const QueuePositionModel&) = delete;

  [[nodiscard]] std::int64_t conservatism_bps() const noexcept { return conservatism_bps_; }
  [[nodiscard]] std::size_t size() const noexcept { return pool_.size(); }

  // Rests an order behind `level_qty` displayed at its price. Invalid handle when full/dup.
  Handle32 place(ClientOrderId id,
                 std::uint64_t order_id,
                 InstrumentId inst,
                 Side side,
                 Price px,
                 Qty qty,
                 Qty level_qty) noexcept {
    if (by_id_.contains(id)) return Handle32{};
    const Handle32 h = pool_.allocate();
    if (!h.valid()) return Handle32{};
    QueuedOrder& o = pool_.get(h);
    o = QueuedOrder{};
    o.cl_ord_id = id;
    o.order_id = order_id;
    o.price = px;
    o.qty = qty;
    o.ahead = level_qty;
    o.instrument = inst;
    o.side = side;
    by_id_.insert(id, h);
    return h;
  }
  [[nodiscard]] Handle32 find(ClientOrderId id) const noexcept {
    const Handle32* h = by_id_.find(id);
    return h == nullptr ? Handle32{} : *h;
  }
  [[nodiscard]] const QueuedOrder& get(Handle32 h) const noexcept { return pool_.get(h); }
  [[nodiscard]] QueuedOrder& get(Handle32 h) noexcept { return pool_.get(h); }
  [[nodiscard]] bool is_live(Handle32 h) const noexcept { return pool_.is_live(h); }
  void remove(Handle32 h) noexcept {
    by_id_.erase(pool_.get(h).cl_ord_id);
    pool_.free(h);
  }
  // Same price and qty <= leaves: keep the queue position (ahead unchanged).
  bool amend_keep_priority(Handle32 h,
                           ClientOrderId new_id,
                           std::uint64_t order_id,
                           Qty qty) noexcept {
    QueuedOrder& o = pool_.get(h);
    if (qty.is_zero() || qty > o.leaves() || by_id_.contains(new_id)) return false;
    by_id_.erase(o.cl_ord_id);
    o.cl_ord_id = new_id;
    o.order_id = order_id;
    o.qty = qty;
    o.cum_qty = Qty{};
    by_id_.insert(new_id, h);
    return true;
  }

  // Displayed quantity at (inst, side, px) changed old -> new.
  void on_level_change(InstrumentId inst, Side side, Price px, Qty old_qty, Qty new_qty) noexcept {
    if (new_qty >= old_qty) return;
    const Qty r = old_qty - new_qty;
    pool_.for_each([&](Handle32, QueuedOrder& o) {
      if (o.instrument != inst || o.side != side || o.price != px || o.ahead.is_zero()) return;
      if (new_qty.is_zero()) {
        o.ahead = Qty{};
        return;
      }
      // share = r * ahead / old * (1 - c)
      const Int128 share = static_cast<Int128>(r.raw) * o.ahead.raw / old_qty.raw *
                           (10'000 - conservatism_bps_) / 10'000;
      const auto cut = Qty::from_raw(static_cast<std::int64_t>(share));
      o.ahead = cut >= o.ahead ? Qty{} : o.ahead - cut;
    });
  }

  // A trade printed at px with the given aggressor side. F(Handle32, QueuedOrder&, Qty fill)
  // is called for every order that executes; the order's cum_qty is already advanced.
  // Fully filled orders must be removed by the caller (after emitting the fill).
  template <class F>
  void on_trade(InstrumentId inst, Price px, Qty qty, Side aggressor, F&& f) noexcept {
    const Side maker_side = opposite(aggressor);
    pool_.for_each([&](Handle32 h, QueuedOrder& o) {
      if (o.instrument != inst || o.side != maker_side || o.leaves().is_zero()) return;
      Qty fill{};
      if (better(aggressor, px, o.price)) {
        // trade-through: price moved past us, our whole level was consumed
        fill = o.leaves();
        o.ahead = Qty{};
      } else if (px == o.price) {
        if (qty > o.ahead) {
          fill = min(qty - o.ahead, o.leaves());
          o.ahead = Qty{};
        } else {
          o.ahead -= qty;
        }
      }
      if (fill.is_positive()) {
        o.cum_qty += fill;
        f(h, o, fill);
      }
    });
  }

  // Ascending handle order. F(Handle32, const QueuedOrder&).
  template <class F>
  void for_each(F&& f) const noexcept {
    pool_.for_each([&](Handle32 h, const QueuedOrder& o) { f(h, o); });
  }

 private:
  std::int64_t conservatism_bps_;
  Pool<QueuedOrder, kMaxQueuedOrders> pool_;
  OpenHashMap<ClientOrderId, Handle32, kMaxQueuedOrders * 2> by_id_;
};

}  // namespace fastmm::sim
