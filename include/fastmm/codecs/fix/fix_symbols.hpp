#pragma once
// FixSymbolTable: Symbol(55) <-> InstrumentId for one FIX session. Filled at startup with add()
// (no allocation: fixed arrays), then read on the hot path by the decoder (find) and the encoder
// (symbol). Open addressing on an FNV-1a hash; the reverse direction is a direct index.
#include "fastmm/core/fixed_string.hpp"
#include "fastmm/core/strong_id.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fastmm::codecs::fix {

class FixSymbolTable {
 public:
  static constexpr std::size_t kCapacity = 256;
  static constexpr std::size_t kSlots = 512;  // power of two, 2x capacity
  static constexpr std::size_t kMaxInstrumentIds = 1024;
  static constexpr std::size_t kMaxSymbolChars = 31;

  // False when full, duplicate symbol or id, symbol empty or too long, or id out of range.
  bool add(std::string_view symbol, InstrumentId id) noexcept {
    if (count_ >= kCapacity || symbol.empty() || symbol.size() > kMaxSymbolChars || !id.valid() ||
        id.value >= kMaxInstrumentIds || by_id_[id.value] != 0 || find(symbol).valid())
      return false;
    const std::uint64_t h = hash(symbol);
    std::size_t s = h & (kSlots - 1);
    while (slots_[s] != 0) s = (s + 1) & (kSlots - 1);
    entries_[count_] = Entry{FixedString<kMaxSymbolChars>(symbol), id};
    slots_[s] = static_cast<std::uint16_t>(count_ + 1);
    by_id_[id.value] = static_cast<std::uint16_t>(count_ + 1);
    ++count_;
    return true;
  }

  // Invalid id when unknown.
  [[nodiscard]] InstrumentId find(std::string_view symbol) const noexcept {
    std::size_t s = hash(symbol) & (kSlots - 1);
    while (slots_[s] != 0) {
      const Entry& e = entries_[slots_[s] - 1U];
      if (e.symbol.view() == symbol) return e.id;
      s = (s + 1) & (kSlots - 1);
    }
    return InstrumentId{};
  }

  // Empty when unknown.
  [[nodiscard]] std::string_view symbol(InstrumentId id) const noexcept {
    if (!id.valid() || id.value >= kMaxInstrumentIds || by_id_[id.value] == 0) return {};
    return entries_[by_id_[id.value] - 1U].symbol.view();
  }

  [[nodiscard]] std::size_t size() const noexcept { return count_; }

 private:
  struct Entry {
    FixedString<kMaxSymbolChars> symbol;
    InstrumentId id;
  };
  static constexpr std::uint64_t hash(std::string_view s) noexcept {
    std::uint64_t h = 14695981039346656037ULL;
    for (const char c : s) {
      h ^= static_cast<unsigned char>(c);
      h *= 1099511628211ULL;
    }
    return h;
  }

  Entry entries_[kCapacity]{};
  std::uint16_t slots_[kSlots]{};
  std::uint16_t by_id_[kMaxInstrumentIds]{};
  std::size_t count_ = 0;
};

}  // namespace fastmm::codecs::fix
