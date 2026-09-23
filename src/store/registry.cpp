#include "fastmm/store/registry.hpp"

#include "fastmm/store/sqlite_store.hpp"

namespace fastmm::store {

StoreRegistry& StoreRegistry::instance() {
  static StoreRegistry r;
  return r;
}

AddResult StoreRegistry::add(std::string_view name,
                             StoreEntry::BackendFactory backend,
                             StoreEntry::ReaderFactory reader) {
  if (name.empty() || name == kNoBackend || backend == nullptr) return AddResult::Invalid;
  for (StoreEntry& e : entries_) {
    if (e.name != name) continue;
    if (e.backend == backend && e.reader == reader) return AddResult::AlreadyPresent;
    return AddResult::Conflict;
  }
  entries_.push_back(StoreEntry{std::string(name), backend, reader});
  return AddResult::Added;
}

const StoreEntry* StoreRegistry::find(std::string_view name) const noexcept {
  for (const StoreEntry& e : entries_) {
    if (e.name == name) return &e;
  }
  return nullptr;
}

std::string StoreRegistry::names() const {
  std::string s(kNoBackend);
  for (const StoreEntry& e : entries_) {
    s += ", ";
    s += e.name;
  }
  return s;
}

std::unique_ptr<Backend> StoreRegistry::make_backend(std::string_view name) const {
  const StoreEntry* e = find(name);
  return e == nullptr ? nullptr : e->backend();
}

std::unique_ptr<Reader> StoreRegistry::make_reader(std::string_view name) const {
  const StoreEntry* e = find(name);
  if (e == nullptr || e->reader == nullptr) return nullptr;
  return e->reader();
}

void register_builtin_backends(StoreRegistry& r) {
  static_cast<void>(r.add("sqlite", &make_sqlite_backend, &make_sqlite_reader));
}

std::string configured_backend(const GenericSection& storage) {
  return storage.get_string("backend", "sqlite");
}

}  // namespace fastmm::store
