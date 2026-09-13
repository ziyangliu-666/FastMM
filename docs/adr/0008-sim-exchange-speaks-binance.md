# ADR-0008: The simulated exchange speaks the Binance protocol

Status: accepted (2026-09)

## Context

The live connector code path (TLS, WS, JSON, sequence sync, auth, order flow) needs an end-to-end test without a real exchange.

## Decision

`fastmm-sim-exchange` serves Binance-compatible REST/WS endpoints (depth diffs with U/u, bookTicker, executionReport, WS API order.place) with fault injection.

## Consequences

The real Binance connector runs unmodified against localhost in CI, including resync after injected gaps.
