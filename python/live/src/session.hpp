#pragma once
// The live runtime of Python hot strategies in fastmm_live._live (session.cpp).
#include <pybind11/pybind11.h>

namespace fastmm::py_live {

void bind_session(pybind11::module_& m);

}  // namespace fastmm::py_live
