# ADR-0001: Fixed-point int64 for Price and Qty

Status: accepted (2026-09)

## Context

Prices and quantities arrive as decimal strings from every venue. Floating point cannot represent them exactly and makes PnL non-associative.

## Decision

`Price`, `Qty`, `Notional` are strong types over `int64_t` with a global 1e-8 scale. Decimal strings parse exactly. Products use `__int128`. Tick and lot rounding are explicit per side.

## Consequences

Range is +/- 9.2e10 units; instruments outside it are rejected at load time. Only the Avellaneda-Stoikov formula uses `double`, on an already converted mid, then rounds to tick.
