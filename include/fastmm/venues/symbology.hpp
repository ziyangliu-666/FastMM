#pragma once
// SymbolTable (6.2): venue symbol <-> InstrumentId. Built once at startup from the
// InstrumentTable, read-only afterwards, so the net threads can share it without locks.
//
// Lookup is an open-addressing table keyed by (VenueId, FNV-1a of the upper-cased symbol);
// venues differ in case (Binance streams are lowercase, REST/user-data payloads uppercase,
// Bybit always uppercase) so the canonical key is uppercase and lookups fold case on the fly
// without allocating.
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/strong_id.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fastmm::venues {

[[nodiscard]] constexpr char ascii_upper(char c) noexcept {
  return (c >= 'a' && c <= 'z') ? static_cast<char>(c - 32) : c;
}
[[nodiscard]] constexpr char ascii_lower(char c) noexcept {
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

// FNV-1a over the upper-cased bytes.
[[nodiscard]] constexpr std::uint64_t symbol_hash(std::string_view s) noexcept {
  std::uint64_t h = 14695981039346656037ULL;
  for (char c : s) {
    h ^= static_cast<std::uint8_t>(ascii_upper(c));
    h *= 1099511628211ULL;
  }
  return h;
}

[[nodiscard]] constexpr bool iequals_symbol(std::string_view a, std::string_view b) noexcept {
  if (a.size() != b.size()) return false;
  for (std::size_t i = 0; i < a.size(); ++i) {
    if (ascii_upper(a[i]) != ascii_upper(b[i])) return false;
  }
  return true;
}

class SymbolTable {
 public:
  static constexpr std::size_t kSlots = 1024;  // >= 4 * kMaxInstruments: load factor <= 0.25
  static_assert((kSlots & (kSlots - 1)) == 0);

  SymbolTable() noexcept { clear(); }

  void clear() noexcept {
    for (Slot& s : slots_) s = Slot{};
    for (Entry& e : by_id_) e = Entry{};
    count_ = 0;
  }

  // Registers every instrument of the table. Returns false on a duplicate (venue, symbol)
  // pair or if the symbol is empty; the table is then partially filled and must be cleared.
  bool build(const InstrumentTable& instruments) noexcept {
    clear();
    for (const Instrument& inst : instruments) {
      if (!add(inst.id, inst.venue, inst.symbol.view())) return false;
    }
    return true;
  }

  bool add(InstrumentId id, VenueId venue, std::string_view symbol) noexcept {
    if (symbol.empty() || symbol.size() > Symbol::kCapacity || id.value >= kMaxInstruments)
      return false;
    if (find(venue, symbol).valid()) return false;
    if (count_ >= kMaxInstruments) return false;
    const std::uint64_t h = symbol_hash(symbol);
    std::size_t i = static_cast<std::size_t>(h) & (kSlots - 1);
    while (slots_[i].used) i = (i + 1) & (kSlots - 1);
    slots_[i] = Slot{h, id, venue, true};
    Entry& e = by_id_[id.value];
    e.venue = venue;
    for (std::size_t k = 0; k < symbol.size(); ++k) {
      e.upper.push_back(ascii_upper(symbol[k]));
      e.lower.push_back(ascii_lower(symbol[k]));
    }
    e.valid = true;
    ++count_;
    return true;
  }

  // Case-insensitive lookup; invalid id when unknown. Hot path: no allocation.
  [[nodiscard]] InstrumentId find(VenueId venue, std::string_view symbol) const noexcept {
    const std::uint64_t h = symbol_hash(symbol);
    std::size_t i = static_cast<std::size_t>(h) & (kSlots - 1);
    for (std::size_t probes = 0; probes < kSlots; ++probes) {
      const Slot& s = slots_[i];
      if (!s.used) return InstrumentId::invalid();
      if (s.hash == h && s.venue == venue &&
          iequals_symbol(by_id_[s.id.value].upper.view(), symbol))
        return s.id;
      i = (i + 1) & (kSlots - 1);
    }
    return InstrumentId::invalid();
  }

  // Canonical (uppercase) venue symbol, e.g. "BTCUSDT"; empty if unknown.
  [[nodiscard]] std::string_view venue_symbol(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && by_id_[id.value].valid ? by_id_[id.value].upper.view()
                                                                : std::string_view{};
  }
  // Lowercase form used by Binance stream names ("btcusdt@depth@100ms").
  [[nodiscard]] std::string_view lower_symbol(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && by_id_[id.value].valid ? by_id_[id.value].lower.view()
                                                                : std::string_view{};
  }
  [[nodiscard]] VenueId venue_of(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && by_id_[id.value].valid ? by_id_[id.value].venue
                                                                : VenueId::invalid();
  }
  [[nodiscard]] bool contains(InstrumentId id) const noexcept {
    return id.value < kMaxInstruments && by_id_[id.value].valid;
  }
  [[nodiscard]] std::size_t size() const noexcept { return count_; }

 private:
  struct Slot {
    std::uint64_t hash = 0;
    InstrumentId id{};
    VenueId venue{};
    bool used = false;
  };
  struct Entry {
    Symbol upper{};
    Symbol lower{};
    VenueId venue{};
    bool valid = false;
  };
  std::array<Slot, kSlots> slots_;
  std::array<Entry, kMaxInstruments> by_id_;
  std::size_t count_ = 0;
};

}  // namespace fastmm::venues
