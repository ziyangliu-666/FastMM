#pragma once
// PaddedJson: owns a copy of a JSON text with kJsonPadding zero bytes behind it so the
// decoders (which assume RecvBuffer-style padding) can be driven from fixtures, tests and
// benchmarks. Never used on the hot path.
#include "fastmm/venues/feed.hpp"

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>

namespace fastmm::venues {

class PaddedJson {
 public:
  PaddedJson() = default;
  explicit PaddedJson(std::string_view text) { assign(text); }

  void assign(std::string_view text) {
    len_ = text.size();
    buf_.reset(new char[len_ + kJsonPadding]);
    if (len_ > 0) std::memcpy(buf_.get(), text.data(), len_);
    std::memset(buf_.get() + len_, 0, kJsonPadding);
  }
  [[nodiscard]] std::string_view view() const noexcept { return {buf_.get(), len_}; }
  [[nodiscard]] const char* data() const noexcept { return buf_.get(); }
  [[nodiscard]] std::size_t size() const noexcept { return len_; }

 private:
  std::unique_ptr<char[]> buf_;
  std::size_t len_ = 0;
};

}  // namespace fastmm::venues
