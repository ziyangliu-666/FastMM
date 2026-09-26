#pragma once
// The SQLite storage backend ([storage] backend = "sqlite"), the one FastMM ships.
//
// One file, WAL, one row per fill, order, position snapshot and kill event, plus a per-day PnL
// roll-up kept as records arrive. The schema and the durability it gives are in
// docs/reference/storage.md; the SQL itself is src/store/sqlite_schema.cpp.
//
// SQLite is bundled (cmake/Dependencies.cmake pins the amalgamation) rather than taken from the
// host: FastMM vendors its dependencies through CPM (ADR-0005), the WAL and UPSERT behaviour the
// store relies on is version-dependent, and a trading host should not need a -dev package.
#include "fastmm/store/backend.hpp"
#include "fastmm/store/reader.hpp"

#include <memory>

namespace fastmm::store {

// Bumped whenever the schema changes; sqlite_schema.cpp holds one migration step per version.
inline constexpr int kSqliteSchemaVersion = 3;

[[nodiscard]] std::unique_ptr<Backend> make_sqlite_backend();
[[nodiscard]] std::unique_ptr<Reader> make_sqlite_reader();

// Where the backend puts its file: [storage] path, else "<default_dir>/<engine_name>.db".
[[nodiscard]] std::string sqlite_path(const BackendOptions& opts);

}  // namespace fastmm::store
