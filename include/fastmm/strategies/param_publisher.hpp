#pragma once
// ParamPublisher: builds ParamUpdate messages for a running engine, off the engine thread
// (ADR-0013).
//
//   MsgRing ring(1U << 16);                             // one of the engine's RingFeed rings
//   ParamPublisher pub(ParamSink::to_ring(ring), strategy.params());
//   pub.publish({{"half_spread_bps", "7.5"}});          // false: no room in the ring
//
// A publish parses the named values into a copy of the parameters with the schema's parsers and
// ranges, then runs the params struct's validate(). An unknown name, a name given twice, a value
// that does not parse or is out of range, a failed validate() and more than
// ParamUpdateMsg::kMaxFields values throw std::invalid_argument; nothing is sent then. The engine
// copies the raw values without checks.
//
// The copy starts from the parameters passed to the constructor (read them before the engine thread
// starts) and follows every update that was sent, so it equals what the engine holds once it has
// consumed the ring. Parameters an update does not name keep their values. With `instruments` > 0
// the publisher keeps one copy per instrument and an update may name one instrument; a strategy
// that keeps one parameter set (StrategyBase) publishes to all instruments.
//
// Calls may come from any thread; a mutex serialises them, so the publisher is the ring's only
// producer. Not for the engine thread: publish allocates.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/params.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fastmm {

// Where a built update goes; push returns false when it was not accepted (full).
struct ParamSink {
  void* ctx = nullptr;
  bool (*push)(void* ctx, const ParamUpdateMsg& m) noexcept = nullptr;

  // The SPSC ring an engine's RingFeed polls.
  [[nodiscard]] static ParamSink to_ring(MsgRing& ring) noexcept {
    return {&ring, [](void* c, const ParamUpdateMsg& m) noexcept {
              return static_cast<MsgRing*>(c)->try_push(&m.hdr, m.hdr.len);
            }};
  }
};

class ParamPublisher {
 public:
  using ParamValue = std::pair<std::string, std::string>;  // name, value as ParamDesc::parse takes
  static constexpr InstrumentId kAllInstruments = ParamUpdateMsg::kAllInstruments;

  template <class Params>
  ParamPublisher(ParamSink sink, const Params& current, std::size_t instruments = 0)
      : sink_(sink), ops_(&block_ops<Params>()), per_instrument_(instruments != 0) {
    const std::size_t n = instruments == 0 ? 1 : instruments;
    blocks_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) blocks_.push_back(clone(&current));
  }
  ParamPublisher(const ParamPublisher&) = delete;
  ParamPublisher& operator=(const ParamPublisher&) = delete;
  ParamPublisher(ParamPublisher&&) = delete;
  ParamPublisher& operator=(ParamPublisher&&) = delete;
  ~ParamPublisher() = default;

  // Validates and sends one update. Throws std::invalid_argument when it is invalid; returns false
  // when the sink refused it (ring full) or the publisher is closed.
  bool publish(const std::vector<ParamValue>& values, InstrumentId inst = kAllInstruments) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    ParamUpdateMsg m{};
    std::vector<std::pair<std::size_t, Block>> next;
    if (auto err = build_locked(values, inst, m, &next)) throw std::invalid_argument(*err);
    m.publish_seq = seq_ + 1;
    m.hdr.recv_ts = wall_now();
    if (!sink_.push(sink_.ctx, m)) {
      ++refused_;
      return false;
    }
    ++seq_;
    for (auto& [k, block] : next) blocks_[k] = std::move(block);
    return true;
  }

  // The update publish() would send, without sending it or changing the copy; the error message
  // when it is invalid. `out.publish_seq` and `out.hdr.recv_ts` stay zero.
  [[nodiscard]] std::optional<std::string> build(const std::vector<ParamValue>& values,
                                                 InstrumentId inst,
                                                 ParamUpdateMsg& out) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return build_locked(values, inst, out, nullptr);
  }

  // Later publishes return false (the session has stopped).
  void close() {
    const std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  [[nodiscard]] bool closed() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }
  // Updates sent, and updates the sink refused.
  [[nodiscard]] std::uint64_t published() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return seq_;
  }
  [[nodiscard]] std::uint64_t refused() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return refused_;
  }
  [[nodiscard]] const ParamSchema& schema() const { return ops_->schema(); }
  // `name=value` pairs of the copy: one instrument's, or the shared set.
  [[nodiscard]] std::string describe(InstrumentId inst = kAllInstruments) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t k =
        per_instrument_ && inst.valid() && inst.value < blocks_.size() ? inst.value : 0;
    return detail::describe_params(ops_->schema(), blocks_[k].get());
  }

 private:
  struct BlockOps {
    const ParamSchema& (*schema)();
    void* (*clone)(const void* src);
    void (*destroy)(void* p) noexcept;
    std::optional<std::string> (*validate)(const void* p);
  };
  template <class P>
  static const BlockOps& block_ops() {
    static constexpr BlockOps kOps{
        []() -> const ParamSchema& { return P::schema(); },
        [](const void* src) -> void* { return new P(*static_cast<const P*>(src)); },
        [](void* p) noexcept { delete static_cast<P*>(p); },
        [](const void* p) { return detail::validate_params(*static_cast<const P*>(p)); }};
    return kOps;
  }
  struct Deleter {
    void (*destroy)(void*) noexcept;
    void operator()(void* p) const noexcept { destroy(p); }
  };
  using Block = std::unique_ptr<void, Deleter>;

  [[nodiscard]] Block clone(const void* src) const {
    return Block(ops_->clone(src), Deleter{ops_->destroy});
  }

  std::optional<std::string> build_locked(const std::vector<ParamValue>& values,
                                          InstrumentId inst,
                                          ParamUpdateMsg& out,
                                          std::vector<std::pair<std::size_t, Block>>* next) const {
    const ParamSchema& schema = ops_->schema();
    if (values.size() > ParamUpdateMsg::kMaxFields) {
      return "at most " + std::to_string(ParamUpdateMsg::kMaxFields) +
             " parameters per update, got " + std::to_string(values.size());
    }
    std::size_t first = 0;
    std::size_t last = blocks_.size();
    if (inst.valid()) {
      if (!per_instrument_) {
        return std::string(
            "the strategy keeps one parameter set for all instruments; publish "
            "without an instrument");
      }
      if (inst.value >= blocks_.size())
        return "instrument " + std::to_string(inst.value) + " is not in the instrument table";
      first = inst.value;
      last = first + 1;
    }
    out = ParamUpdateMsg{};
    init_header(out, EventType::ParamUpdate, inst);
    out.hdr.flags = EventHeader::kSynthetic;
    out.count = static_cast<std::uint32_t>(values.size());
    for (std::size_t k = first; k < last; ++k) {
      Block copy = clone(blocks_[k].get());
      for (std::size_t i = 0; i < values.size(); ++i) {
        const auto& [name, value] = values[i];
        const ParamDesc* d = schema.find(name);
        if (d == nullptr) return "unknown parameter '" + name + "'";
        for (std::size_t j = 0; j < i; ++j) {
          if (values[j].first == name) return "parameter '" + name + "' given twice";
        }
        if (auto err = d->parse(copy.get(), value)) return "parameter '" + name + "': " + *err;
        out.field[i] = static_cast<std::uint16_t>(d - schema.begin());
        out.value[i] = d->get_raw(copy.get());
      }
      if (auto err = ops_->validate(copy.get())) {
        if (per_instrument_) return "instrument " + std::to_string(k) + ": " + *err;
        return err;
      }
      if (next != nullptr) next->emplace_back(k, std::move(copy));
    }
    return std::nullopt;
  }

  ParamSink sink_;
  const BlockOps* ops_;
  bool per_instrument_;
  std::vector<Block> blocks_;  // one per instrument, or the shared set
  mutable std::mutex mutex_;
  std::uint64_t seq_ = 0;
  std::uint64_t refused_ = 0;
  bool closed_ = false;
};

}  // namespace fastmm
