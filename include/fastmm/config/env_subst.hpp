#pragma once
// ${VAR} environment substitution for [venues.*] string values (8.7). Only exact
// `${NAME}` tokens are replaced; a missing variable is an error (never silently empty).
#include "fastmm/core/result.hpp"

#include <cstdlib>
#include <string>
#include <string_view>

namespace fastmm {

// Returns the substituted string, or the name of the first missing variable.
inline Result<std::string, std::string> substitute_env(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  std::size_t i = 0;
  while (i < in.size()) {
    if (in[i] == '$' && i + 1 < in.size() && in[i + 1] == '{') {
      const std::size_t close = in.find('}', i + 2);
      if (close == std::string_view::npos)
        return fail(std::string("unterminated ${ in '") + std::string(in) + "'");
      const std::string name(in.substr(i + 2, close - i - 2));
      const char* v = name.empty() ? nullptr : std::getenv(name.c_str());
      if (v == nullptr) return fail(name);
      out += v;
      i = close + 1;
      continue;
    }
    out.push_back(in[i]);
    ++i;
  }
  return out;
}

[[nodiscard]] inline bool has_env_reference(std::string_view s) noexcept {
  return s.find("${") != std::string_view::npos;
}

}  // namespace fastmm
