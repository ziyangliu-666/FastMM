// fastmm.open_store()'s P&L (python/fastmm/store.py): the SQLite reader fastmm-pnl uses
// (src/store/sqlite_reader.cpp), so the fill ledger is computed in one place and the schema version
// the module accepts is the one the engine writes.
#include "bind_common.hpp"

#include "fastmm/config/config.hpp"
#include "fastmm/store/backend.hpp"
#include "fastmm/store/reader.hpp"
#include "fastmm/store/sqlite_store.hpp"

#include <pybind11/stl.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::py_bind {

void bind_store(py::module_& m) {
  m.attr("_store_schema_version") = store::kSqliteSchemaVersion;
  m.def(
      "_store_pnl",
      [](const py::object& path,
         const std::string& instrument,
         const std::string& from,
         const std::string& to) {
        GenericSection section;
        section.values["path"] = fspath(path);
        store::Rows rows;
        std::string error;
        {
          const py::gil_scoped_release release;
          store::BackendOptions opts;
          opts.config = &section;
          opts.read_only = true;
          const std::unique_ptr<store::Reader> reader = store::make_sqlite_reader();
          if (auto opened = reader->open(opts); !opened) {
            error = opened.error();
          } else {
            store::QueryFilter f;
            f.instrument = instrument;
            f.from = from;
            f.to = to;
            auto r = reader->pnl(f);
            if (r) {
              rows = std::move(*r);
            } else {
              error = r.error();
            }
          }
        }
        if (!error.empty()) throw py::value_error(error);
        return py::make_tuple(std::move(rows.columns), std::move(rows.rows));
      },
      py::arg("path"),
      py::arg("instrument") = "",
      py::arg("from_day") = "",
      py::arg("to_day") = "",
      "Internal: (columns, rows) of fastmm-pnl's pnl query; raw columns as exact decimals.");
}

}  // namespace fastmm::py_bind
