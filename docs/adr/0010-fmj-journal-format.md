# ADR-0010: The .fmj journal format

Status: accepted (2026-09)

## Context

Deterministic replay needs every inbound event (market data, order events, timers, connection states) in the exact consumption order.

## Decision

Append-only mmap file: header (magic FMJ1, session, TSC calibration, instrument table, config hash) + 1 MiB blocks with CRC32C. The engine assigns a global sequence number as it consumes.

## Consequences

A truncated tail block is detected by CRC and dropped. Journals are self-contained inputs for `fastmm-backtest` and `fastmm-replay`.
