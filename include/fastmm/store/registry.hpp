#pragma once
// Storage backend registry: name -> factories. [storage] backend names one entry.
//
//   fastmm::store::StoreRegistry::instance().add("clickhouse", &make_ch_backend, &make_ch_reader);
//
// Nothing registers itself, for the same reason the strategy registry does not (a static library's
// self-registering object is dropped by the linker): fastmm::store::register_builtin_backends()
// adds the ones FastMM ships, and an out-of-tree backend calls add() before starting a session.
// The name "none" is reserved and never reaches the registry: it means no backend at all.
#include "fastmm/store/backend.hpp"
#include "fastmm/store/reader.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::store {

inline constexpr std::string_view kNoBackend = "none";

struct StoreEntry {
  using BackendFactory = std::unique_ptr<Backend> (*)();
  using ReaderFactory = std::unique_ptr<Reader> (*)();
  std::string name;
  BackendFactory backend = nullptr;
  ReaderFactory reader = nullptr;  // null: the backend records but cannot be queried in process
};

enum class AddResult : std::uint8_t { Added, AlreadyPresent, Conflict, Invalid };
[[nodiscard]] constexpr std::string_view to_string(AddResult r) noexcept {
  switch (r) {
    case AddResult::Added:
      return "added";
    case AddResult::AlreadyPresent:
      return "already present";
    case AddResult::Conflict:
      return "conflict";
    case AddResult::Invalid:
      return "invalid";
  }
  return "?";
}

class StoreRegistry {
 public:
  // The registry fastmm-live and fastmm-pnl use. Not thread-safe for writers: register before
  // starting threads. Tests use a local registry.
  static StoreRegistry& instance();

  AddResult add(std::string_view name,
                StoreEntry::BackendFactory backend,
                StoreEntry::ReaderFactory reader = nullptr);
  [[nodiscard]] const StoreEntry* find(std::string_view name) const noexcept;
  [[nodiscard]] const std::vector<StoreEntry>& entries() const noexcept { return entries_; }
  // Comma-separated names, for an error message; always starts with "none".
  [[nodiscard]] std::string names() const;
  // Null when `name` is unknown or, for make_reader, when the backend cannot be queried.
  [[nodiscard]] std::unique_ptr<Backend> make_backend(std::string_view name) const;
  [[nodiscard]] std::unique_ptr<Reader> make_reader(std::string_view name) const;

 private:
  std::vector<StoreEntry> entries_;
};

// Adds the backends FastMM ships ("sqlite"). Idempotent.
void register_builtin_backends(StoreRegistry& r = StoreRegistry::instance());

// [storage] backend, defaulting to "sqlite"; "none" disables the store.
[[nodiscard]] std::string configured_backend(const GenericSection& storage);

}  // namespace fastmm::store
