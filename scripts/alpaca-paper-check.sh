#!/usr/bin/env bash
# The Alpaca connector against a paper account (opt-in; tests/venues/live_alpaca_test.cpp): on SPY
# it places, replaces and cancels a far buy, buys and sells one share, reconnects, starts a second
# connector over a resting order and cancels everything. Needs ALPACA_API_KEY and
# ALPACA_API_SECRET of a paper account and a build; runs 04:00 to 20:00 New York time.
#   ./scripts/alpaca-paper-check.sh [build-dir]        # default build/release
set -euo pipefail
cd "$(dirname "$0")/.."
BUILD="${1:-build/release}"
: "${ALPACA_API_KEY:?export ALPACA_API_KEY (paper account)}"
: "${ALPACA_API_SECRET:?export ALPACA_API_SECRET (paper account)}"
FASTMM_LIVE_TESTS=1 "$BUILD/tests/fastmm_venues_tests" -tc='live.alpaca*'
