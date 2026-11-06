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
// the publisher keeps one copy per instrument, and one of the set the updates for every instrument
// built, and an update may name one instrument, whose values then stay its own until a reset
// (strategies/param_overrides.hpp); a strategy that keeps one parameter set (ParamScope::Shared)
// publishes to all instruments.
//
// Calls may come from any thread; a mutex serialises them, so the publisher is the ring's only
// producer. Not for the engine thread: publish allocates.
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/param_overrides.hpp"
#include "fastmm/strategies/params.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
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

  // Copying and freeing a parameter block whose layout is known only at run time
  // (strategies/hot_params.hpp); validate runs after the schema's parsers.
  struct BlockOps {
    void* (*clone)(const void* src);
    void (*destroy)(void* p) noexcept;
    std::optional<std::string> (*validate)(const void* p);
  };

  template <class Params>
  ParamPublisher(ParamSink sink, const Params& current, std::size_t instruments = 0)
      : ParamPublisher(sink, Params::schema(), block_ops<Params>(), &current, instruments) {}
  // `schema`'s parsers and `ops` work on blocks like `current`; `schema` must outlive the
  // publisher.
  ParamPublisher(ParamSink sink,
                 const ParamSchema& schema,
                 const BlockOps& ops,
                 const void* current,
                 std::size_t instruments = 0)
      : sink_(sink), schema_(&schema), ops_(ops), per_instrument_(instruments != 0) {
    const std::size_t n = instruments == 0 ? 1 : instruments + 1;  // + the shared set, last
    blocks_.reserve(n);
    for (std::size_t i = 0; i < n; ++i) blocks_.push_back(clone(current));
    own_.assign(instruments, ParamMask{});
  }
  ParamPublisher(const ParamPublisher&) = delete;
  ParamPublisher& operator=(const ParamPublisher&) = delete;
  ParamPublisher(ParamPublisher&&) = delete;
  ParamPublisher& operator=(ParamPublisher&&) = delete;
  ~ParamPublisher() = default;

  // Validates and sends one update. Throws std::invalid_argument when it is invalid; returns false
  // when the sink refused it (ring full) or the publisher is closed.
  bool publish(const std::vector<ParamValue>& values, InstrumentId inst = kAllInstruments) {
    return publish_as(values, inst, ParamUpdateMsg::Origin::Strategy, {}) != 0;
  }
  // publish() with the update's origin, source (ParamUpdateMsg::source, cut to 39 bytes), [ab]
  // arm (ParamUpdateMsg::arm) and flags. With ParamUpdateMsg::kReset and an instrument, the values
  // are ignored: the named parameters take the shared values again. The update's publish_seq, 0
  // when the sink refused it or the publisher is closed.
  std::uint64_t publish_as(const std::vector<ParamValue>& values,
                           InstrumentId inst,
                           ParamUpdateMsg::Origin origin,
                           std::string_view source,
                           std::uint8_t arm = ParamUpdateMsg::kArmKeep,
                           std::uint8_t flags = 0) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return 0;
    ParamUpdateMsg m{};
    Next next;
    if (auto err = build_locked(values, inst, arm, flags, m, &next))
      throw std::invalid_argument(*err);
    m.publish_seq = seq_ + 1;
    m.origin = origin;
    m.set_source(source);
    m.hdr.recv_ts = wall_now();
    if (!sink_.push(sink_.ctx, m)) {
      ++refused_;
      return 0;
    }
    ++seq_;
    for (auto& [k, block] : next.blocks) blocks_[k] = std::move(block);
    own_ = std::move(next.own);
    return seq_;
  }
  // The named parameters of `inst` take the shared values again; for kAllInstruments, every
  // instrument does: publish_as() with ParamUpdateMsg::kReset.
  std::uint64_t reset(const std::vector<std::string>& names,
                      InstrumentId inst,
                      ParamUpdateMsg::Origin origin,
                      std::string_view source) {
    std::vector<ParamValue> values;
    values.reserve(names.size());
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const std::string& name : names) {
        const ParamDesc* d = schema_->find(name);
        values.emplace_back(name, d == nullptr ? std::string() : d->format(blocks_.back().get()));
      }
    }
    return publish_as(
        values, inst, origin, source, ParamUpdateMsg::kArmKeep, ParamUpdateMsg::kReset);
  }

  // The update publish() would send, without sending it or changing the copy; the error message
  // when it is invalid. `out.publish_seq` and `out.hdr.recv_ts` stay zero.
  [[nodiscard]] std::optional<std::string> build(const std::vector<ParamValue>& values,
                                                 InstrumentId inst,
                                                 ParamUpdateMsg& out,
                                                 std::uint8_t arm = ParamUpdateMsg::kArmKeep,
                                                 std::uint8_t flags = 0) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return build_locked(values, inst, arm, flags, out, nullptr);
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
  [[nodiscard]] const ParamSchema& schema() const { return *schema_; }
  // An update may name one instrument: the strategy keeps a parameter set per instrument.
  [[nodiscard]] bool per_instrument() const noexcept { return per_instrument_; }
  // Every parameter of the copy, in schema order: one instrument's, or the shared set.
  struct Value {
    std::string name;
    ParamType type;
    std::int64_t raw;  // ParamDesc::get_raw
    std::string text;  // ParamDesc::format
  };
  [[nodiscard]] std::vector<Value> values(InstrumentId inst = kAllInstruments) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t k = block_of(inst);
    std::vector<Value> out;
    out.reserve(schema_->size());
    for (const ParamDesc& d : *schema_)
      out.push_back({d.name, d.type, d.get_raw(blocks_[k].get()), d.format(blocks_[k].get())});
    return out;
  }
  // `name=value` pairs of the copy: one instrument's, or the shared set.
  [[nodiscard]] std::string describe(InstrumentId inst = kAllInstruments) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return detail::describe_params(*schema_, blocks_[block_of(inst)].get());
  }
  // The parameters the instrument holds values of its own for, in schema order; none for
  // kAllInstruments.
  [[nodiscard]] std::vector<std::string> own(InstrumentId inst) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> out;
    if (!inst.valid() || inst.value >= own_.size()) return out;
    for (std::size_t f = 0; f < schema_->size(); ++f) {
      if (own_[inst.value].test(f)) out.emplace_back(schema_->begin()[f].name);
    }
    return out;
  }

 private:
  template <class P>
  static const BlockOps& block_ops() {
    static constexpr BlockOps kOps{
        [](const void* src) -> void* { return new P(*static_cast<const P*>(src)); },
        [](void* p) noexcept { delete static_cast<P*>(p); },
        [](const void* p) { return detail::validate_params(*static_cast<const P*>(p)); }};
    return kOps;
  }
  // The copy of one instrument, or the shared set.
  [[nodiscard]] std::size_t block_of(InstrumentId inst) const noexcept {
    if (!per_instrument_) return 0;
    return inst.valid() && inst.value + 1 < blocks_.size() ? inst.value : blocks_.size() - 1;
  }
  struct Deleter {
    void (*destroy)(void*) noexcept;
    void operator()(void* p) const noexcept { destroy(p); }
  };
  using Block = std::unique_ptr<void, Deleter>;

  [[nodiscard]] Block clone(const void* src) const {
    return Block(ops_.clone(src), Deleter{ops_.destroy});
  }

  struct Next {
    std::vector<std::pair<std::size_t, Block>> blocks;
    std::vector<ParamMask> own;
  };

  std::optional<std::string> build_locked(const std::vector<ParamValue>& values,
                                          InstrumentId inst,
                                          std::uint8_t arm,
                                          std::uint8_t flags,
                                          ParamUpdateMsg& out,
                                          Next* next) const {
    const ParamSchema& schema = *schema_;
    if (values.size() > ParamUpdateMsg::kMaxFields) {
      return "at most " + std::to_string(ParamUpdateMsg::kMaxFields) +
             " parameters per update, got " + std::to_string(values.size());
    }
    const std::size_t shared = blocks_.size() - 1;
    if (inst.valid()) {
      if (!per_instrument_) {
        return std::string(
            "the strategy keeps one parameter set for all instruments; publish "
            "without an instrument");
      }
      if (inst.value >= shared)
        return "instrument " + std::to_string(inst.value) + " is not in the instrument table";
    }
    out = ParamUpdateMsg{};
    init_header(out, EventType::ParamUpdate, inst);
    out.hdr.flags = EventHeader::kSynthetic;
    out.count = static_cast<std::uint32_t>(values.size());
    out.arm = arm;
    out.flags = flags;
    // The pairs: parsed on a copy of the shared set, or for a reset of one instrument, the shared
    // set's values.
    const bool reset_one = inst.valid() && (flags & ParamUpdateMsg::kReset) != 0;
    const Block scratch = clone(blocks_[shared].get());
    for (std::size_t i = 0; i < values.size(); ++i) {
      const auto& [name, value] = values[i];
      const ParamDesc* d = schema.find(name);
      if (d == nullptr) return "unknown parameter '" + name + "'";
      for (std::size_t j = 0; j < i; ++j) {
        if (values[j].first == name) return "parameter '" + name + "' given twice";
      }
      if (!reset_one) {
        if (auto err = d->parse(scratch.get(), value)) return "parameter '" + name + "': " + *err;
      }
      out.field[i] = static_cast<std::uint16_t>(d - schema.begin());
      out.value[i] = d->get_raw(reset_one ? blocks_[shared].get() : scratch.get());
    }
    // The copies the update changes, by the engine's rule (apply_param_pairs).
    std::vector<std::pair<std::size_t, Block>> changed;
    std::vector<ParamMask> own = own_;
    if (per_instrument_) {
      std::vector<void*> copy(shared, nullptr);
      apply_param_pairs(
          out, std::span<ParamMask>(own), schema.size(), [&](std::size_t k, std::size_t i) {
            if (copy[k] == nullptr) {
              changed.emplace_back(k, clone(blocks_[k].get()));
              copy[k] = changed.back().second.get();
            }
            schema.begin()[out.field[i]].set_raw(copy[k], out.value[i]);
          });
    }
    if (!inst.valid()) {
      changed.emplace_back(shared, clone(blocks_[shared].get()));
      for (std::size_t i = 0; i < values.size(); ++i)
        schema.begin()[out.field[i]].set_raw(changed.back().second.get(), out.value[i]);
    }
    for (const auto& [k, block] : changed) {
      if (auto err = ops_.validate(block.get())) {
        if (k < shared) return "instrument " + std::to_string(k) + ": " + *err;
        return err;
      }
    }
    if (next != nullptr) {
      next->blocks = std::move(changed);
      next->own = std::move(own);
    }
    return std::nullopt;
  }

  ParamSink sink_;
  const ParamSchema* schema_;
  BlockOps ops_;
  bool per_instrument_;
  std::vector<Block> blocks_;   // one per instrument and the shared set last, or the shared set
  std::vector<ParamMask> own_;  // per instrument: the parameters it holds as its own
  mutable std::mutex mutex_;
  std::uint64_t seq_ = 0;
  std::uint64_t refused_ = 0;
  bool closed_ = false;
};

}  // namespace fastmm
