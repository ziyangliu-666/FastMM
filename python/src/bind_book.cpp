// OrderBook: research wrapper over L2Book<256> with float price/quantity I/O. Levels are
// converted to 1e-8 fixed point on the way in (Price::from_double) and applied level by level
// with L2Book::apply_level, so no BookDeltaMsg is ever built here.
#include "bind_common.hpp"

#include "fastmm/core/book/book_features.hpp"
#include "fastmm/core/book/l2_book.hpp"

#include <pybind11/stl.h>

#include <cmath>
#include <cstddef>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace fastmm::py_bind {

namespace {

// Largest magnitude representable in 1e-8 fixed point (with headroom for rounding).
constexpr double kMaxFixed = 9.2e10;

using Book = L2Book<256>;

struct OrderBook {
  Book book;
};

std::vector<Level> to_levels(const py::object& obj, const char* what) {
  std::vector<Level> out;
  if (obj.is_none()) return out;
  using Arr = py::array_t<double, py::array::c_style | py::array::forcecast>;
  Arr a = Arr::ensure(obj);
  if (!a) {
    PyErr_Clear();
    throw py::type_error(std::string(what) +
                         ": expected an (n, 2) array or a sequence of (price, qty) pairs");
  }
  if (a.size() == 0) return out;
  if (a.ndim() != 2 || a.shape(1) != 2) {
    throw py::value_error(std::string(what) + ": expected shape (n, 2) of (price, qty), got ndim " +
                          std::to_string(a.ndim()));
  }
  const auto n = static_cast<std::size_t>(a.shape(0));
  const double* p = a.data();
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    const double px = p[2 * i];
    const double q = p[2 * i + 1];
    if (!std::isfinite(px) || px <= 0.0 || px > kMaxFixed) {
      throw py::value_error(std::string(what) + "[" + std::to_string(i) +
                            "]: price must be positive and finite");
    }
    if (!std::isfinite(q) || q < 0.0 || q > kMaxFixed) {
      throw py::value_error(std::string(what) + "[" + std::to_string(i) +
                            "]: qty must be >= 0 and finite");
    }
    out.push_back(Level{Price::from_double(px), Qty::from_double(q)});
  }
  return out;
}

py::object level_tuple(const Level& l) {
  if (l.price.is_zero() && l.qty.is_zero()) return py::none();
  return py::make_tuple(l.price.to_double(), l.qty.to_double());
}

py::object price_or_none(const Book& b, Price p) {
  if (b.depth(Side::Buy) == 0 || b.depth(Side::Sell) == 0) return py::none();
  return py::float_(p.to_double());
}

py::array side_levels(const Book& b, Side side, const std::optional<std::size_t>& n) {
  std::size_t k = b.depth(side);
  if (n && *n < k) k = *n;
  py::array_t<double> out({static_cast<py::ssize_t>(k), static_cast<py::ssize_t>(2)});
  double* d = out.mutable_data();
  for (std::size_t i = 0; i < k; ++i) {
    const Level l = b.level(side, i);
    d[2 * i] = l.price.to_double();
    d[2 * i + 1] = l.qty.to_double();
  }
  return out;
}

std::size_t check_levels(std::size_t levels) {
  if (levels == 0) throw py::value_error("levels must be >= 1");
  return levels;
}

}  // namespace

void bind_book(py::module_& m) {
  py::class_<OrderBook>(m,
                        "OrderBook",
                        "Price-aggregated L2 book (up to 256 levels per side) for research. "
                        "Prices and quantities are floats, stored in 1e-8 fixed point; "
                        "best-first level order.")
      .def(py::init<>())
      .def(
          "apply_snapshot",
          [](OrderBook& self, const py::object& bids, const py::object& asks) {
            const std::vector<Level> b = to_levels(bids, "bids");
            const std::vector<Level> a = to_levels(asks, "asks");
            self.book.apply_snapshot(b, a, 0, Timestamp{});
          },
          py::arg("bids"),
          py::arg("asks"),
          "Replace the book. bids/asks: (n, 2) float arrays or sequences of (price, qty).")
      .def(
          "apply_delta",
          [](OrderBook& self, const py::object& bids, const py::object& asks) {
            const std::vector<Level> b = to_levels(bids, "bids");
            const std::vector<Level> a = to_levels(asks, "asks");
            for (const Level& l : b)
              static_cast<void>(self.book.apply_level(Side::Buy, l.price, l.qty));
            for (const Level& l : a)
              static_cast<void>(self.book.apply_level(Side::Sell, l.price, l.qty));
          },
          py::arg("bids") = py::none(),
          py::arg("asks") = py::none(),
          "Set/update levels; qty 0 deletes the level.")
      .def("clear", [](OrderBook& self) { self.book.clear(); })
      .def(
          "best_bid",
          [](const OrderBook& self) { return level_tuple(self.book.best_bid()); },
          "(price, qty) of the best bid, or None.")
      .def(
          "best_ask",
          [](const OrderBook& self) { return level_tuple(self.book.best_ask()); },
          "(price, qty) of the best ask, or None.")
      .def(
          "mid",
          [](const OrderBook& self) { return price_or_none(self.book, self.book.mid()); },
          "(best bid + best ask) / 2, or None if a side is empty.")
      .def(
          "spread",
          [](const OrderBook& self) { return price_or_none(self.book, self.book.spread()); },
          "best ask - best bid, or None if a side is empty.")
      .def(
          "microprice",
          [](const OrderBook& self) { return price_or_none(self.book, microprice(self.book)); },
          "Top-of-book quantity-weighted mid, or None if a side is empty.")
      .def(
          "weighted_mid",
          [](const OrderBook& self, std::size_t levels) {
            return price_or_none(self.book, weighted_mid(self.book, check_levels(levels)));
          },
          py::arg("levels") = 5,
          "Mid of the per-side quantity-weighted prices over the top `levels`.")
      .def(
          "imbalance",
          [](const OrderBook& self, std::size_t levels) {
            return imbalance(self.book, check_levels(levels)).to_double();
          },
          py::arg("levels") = 1,
          "(bid qty - ask qty) / (bid qty + ask qty) over the top `levels`, in [-1, 1].")
      .def(
          "bids",
          [](const OrderBook& self, std::optional<std::size_t> n) {
            return side_levels(self.book, Side::Buy, n);
          },
          py::arg("n") = py::none(),
          "Top n bid levels (all if None) as an (k, 2) float64 array, best first.")
      .def(
          "asks",
          [](const OrderBook& self, std::optional<std::size_t> n) {
            return side_levels(self.book, Side::Sell, n);
          },
          py::arg("n") = py::none(),
          "Top n ask levels (all if None) as an (k, 2) float64 array, best first.")
      .def_property_readonly("bid_depth",
                             [](const OrderBook& self) { return self.book.depth(Side::Buy); })
      .def_property_readonly("ask_depth",
                             [](const OrderBook& self) { return self.book.depth(Side::Sell); })
      .def_property_readonly("crossed", [](const OrderBook& self) { return self.book.crossed(); })
      .def_property_readonly(
          "truncated",
          [](const OrderBook& self) { return self.book.truncated(); },
          "A side overflowed 256 levels since the last snapshot.")
      .def("__repr__", [](const OrderBook& self) {
        const Level b = self.book.best_bid();
        const Level a = self.book.best_ask();
        return "<OrderBook bids=" + std::to_string(self.book.depth(Side::Buy)) +
               " asks=" + std::to_string(self.book.depth(Side::Sell)) +
               " best_bid=" + std::to_string(b.price.to_double()) +
               " best_ask=" + std::to_string(a.price.to_double()) + ">";
      });
}

}  // namespace fastmm::py_bind
