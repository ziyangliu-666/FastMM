#!/usr/bin/env bash
# clang-format all tracked C++ sources. --check exits non-zero on differences.
set -euo pipefail
cd "$(dirname "$0")/.."
CF="${CLANG_FORMAT:-$(command -v clang-format-18 || command -v clang-format)}"
FILES=$(git ls-files '*.cpp' '*.hpp' '*.h' '*.cc' 2>/dev/null || find include src apps tests bench python examples -name '*.[ch]pp')
if [[ "${1:-}" == "--check" ]]; then
  echo "$FILES" | xargs -r "$CF" --dry-run -Werror
else
  echo "$FILES" | xargs -r "$CF" -i
fi
