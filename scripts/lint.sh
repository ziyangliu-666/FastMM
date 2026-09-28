#!/usr/bin/env bash
# The CI lint job, locally: formatting, generated code, forbidden patterns, docs. With a build
# directory it also checks the command-line reference against that build's binaries.
#   scripts/lint.sh [build/<preset>]
set -euo pipefail
cd "$(dirname "$0")/.."

CLANG_FORMAT="${CLANG_FORMAT:-clang-format-18}" ./scripts/format.sh --check

python3 tools/sbe_gen.py generate --schema tools/sbe/mdp3_templates_subset.xml \
  --schema-label tools/sbe/mdp3_templates_subset.xml \
  --out include/fastmm/codecs/mdp3/generated/mdp3_schema.hpp \
  --namespace fastmm::codecs::mdp3::schema --check
python3 tools/sbe_gen.py generate --schema tools/sbe/binance_spot_stream_1_0.xml \
  --schema-label tools/sbe/binance_spot_stream_1_0.xml \
  --out include/fastmm/venues/binance/generated/binance_stream_sbe.hpp \
  --namespace fastmm::venues::binance::sbe_stream --check

! grep -rnE 'dynamic_cast|std::cout' include src || { echo "forbidden pattern in hot-path code"; exit 1; }
! grep -rnE 'TEST_CASE\("[^"]*;' tests || { echo "';' in a TEST_CASE name breaks doctest_discover_tests filters"; exit 1; }

python3 tools/doc_snippets.py --check
python3 tools/docs_links.py --check
python3 tools/docs_config_ref.py --check
if [ $# -ge 1 ]; then python3 tools/docs_cli_help.py --check --bin "$1/bin"; fi
echo "lint: ok"
