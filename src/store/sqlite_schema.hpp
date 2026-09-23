#pragma once
// Schema and migrations of the SQLite backend, and the small helpers its two halves share.
// Internal to src/store; not installed.
#include "fastmm/core/result.hpp"

#include <sqlite3.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace fastmm::store::sqlite {

struct Migration {
  int version;           // the version the database is at once `sql` has run
  std::string_view sql;  // one or more statements, run in one transaction
};

// Index 0 brings an empty database to version 1, index 1 to version 2, and so on.
[[nodiscard]] std::span<const Migration> migrations() noexcept;

// Runs every migration above the database's current version, up to `target` (0: the newest), and
// records it in schema_version. A database newer than this build is refused.
[[nodiscard]] Result<int, std::string> migrate(sqlite3* db, int target = 0);
// The version recorded in schema_version, or 0 when the table does not exist.
[[nodiscard]] Result<int, std::string> schema_version(sqlite3* db);

// Runs `sql` for its side effect.
[[nodiscard]] Result<void, std::string> exec(sqlite3* db, const char* sql);
// sqlite3_errmsg with the statement that failed.
[[nodiscard]] std::string error_of(sqlite3* db, std::string_view what);

// "YYYY-MM-DD" (UTC) of a nanosecond wall clock. Negative and zero timestamps give "1970-01-01".
[[nodiscard]] std::string utc_day(std::int64_t ns);
// "YYYY-MM-DD HH:MM:SS" (UTC); empty for a zero timestamp.
[[nodiscard]] std::string utc_stamp(std::int64_t ns);
// The exact decimal of a raw 1e-8 fixed-point value ("1.5", "-0.00000001", "0").
[[nodiscard]] std::string decimal(std::int64_t raw);
// Inclusive UTC day bounds as nanoseconds; `day` is "YYYY-MM-DD". Returns false if it does not
// parse, leaving the outputs alone.
[[nodiscard]] bool day_bounds(std::string_view day, std::int64_t& first_ns, std::int64_t& last_ns);

}  // namespace fastmm::store::sqlite
