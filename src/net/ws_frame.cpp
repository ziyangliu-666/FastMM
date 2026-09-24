#include "fastmm/net/ws_frame.hpp"

#include <simdjson.h>

namespace fastmm::net {

bool ws_utf8_valid(std::span<const std::byte> text) noexcept {
  return simdjson::validate_utf8(reinterpret_cast<const char*>(text.data()), text.size());
}

}  // namespace fastmm::net
