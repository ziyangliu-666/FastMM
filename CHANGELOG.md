# Changelog

All notable changes are recorded here (Keep a Changelog format).

## [Unreleased]

### Added
- Repository skeleton: CMake + CPM with pinned dependencies, presets (debug, release, release-native,
  asan, tsan, coverage, clang, python), GitHub Actions CI, clang-format / clang-tidy, pre-commit.
- `fastmm::core`: fixed-point `Price`/`Qty`/`Notional`, TSC clock, lock-free SPSC rings, `.fmj`
  journal, async logger, latency histograms, L2 and L3 order books with snapshot/delta sync, OMS,
  risk engine, quote manager, `Engine<Strategy, Clock, Transport, Feed>`, BasicMM and
  Avellaneda-Stoikov strategies, TOML configuration with env-var secrets.
- `fastmm::net`: epoll reactor, non-blocking TCP, OpenSSL TLS over BIO pairs, WebSocket client and
  server, HTTP/1.1 client and server, reconnecting connection state machine.
- Documentation: architecture overview, ADR 0001-0011, configuration, venue and strategy guides.
