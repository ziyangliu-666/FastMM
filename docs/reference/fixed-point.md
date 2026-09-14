# Fixed point

Prices, quantities and money are 64-bit integers with a fixed scale of 1e-8 (ADR-0001). The
headers are `include/fastmm/core/fixed_point.hpp` and `include/fastmm/strategies/quoting.hpp`;
`include/fastmm/strategy.hpp` includes both.

## Types

| Type | Meaning | 1 raw unit | Range |
|---|---|---|---|
| `Price` | price per unit | 0.00000001 | +-92,233,720,368.54775807 |
| `Qty` | quantity, signed for positions | 0.00000001 | same |
| `Notional` | price times quantity, fees, PnL | 0.00000001 | same |
| `Ratio` | dimensionless factor; 1.0 is raw 100,000,000 | 0.0001 bp | +-922,337,203 x |

The types do not mix: `Price * Qty` and `Price + Qty` do not compile.
`.raw` is the integer; `from_raw`, `from_int`, `zero`, `max` and `min` build values.

## Arithmetic

| Expression | Result | Rounding |
|---|---|---|
| `a + b`, `a - b`, `-a`, `a.abs()` (same type) | same type | exact |
| `a * k`, `k * a`, `a / k` (`k` is `int64_t`) | same type | `/` truncates toward zero |
| `a / b` (same type) | `int64_t`, e.g. whole ticks | truncates toward zero |
| `a % b` (same type) | same type | C++ `%` |
| `mul(Price, Qty)` | `Notional` | Int128, truncates toward zero |
| `div(Notional, Qty)` | `Price` | Int128, truncates toward zero; zero for a zero quantity |
| `v * r`, `r * v` (`v` any fixed type, `r` a `Ratio`) | the type of `v` | Int128, one truncation toward zero |
| `r1 * r2` | `Ratio` | Int128, truncates toward zero |
| `ratio(num, den)` (same type) | `Ratio` | Int128, truncates toward zero; zero for a zero `den` |
| `r * k` (`k` is `int64_t`) | `Ratio` | exact; must fit int64 |

Truncation toward zero is sign-symmetric, so `mid - mid * (skew * units)` shifts a long and a short
position by the same amount in opposite directions.

To the tick and lot grid, rounding is passive:

| Function | Rule |
|---|---|
| `inst.round_price(p, Side::Buy)` / `round_to_tick(p, tick, Buy)` | down (floor) |
| `inst.round_price(p, Side::Sell)` | up (ceiling) |
| `round_to_tick_nearest(p, tick)` | nearest, half away from zero |
| `inst.round_qty(q)` / `round_to_lot(q, lot)` | down |
| `inst.ticks(n)` | `tick * n` as a price distance |

## Literals

```cpp
using namespace fastmm::literals;   // `using namespace fastmm;` includes them too

const Price p = 100.25_px;          // raw 10'025'000'000
const Qty lot = 0.00000001_qty;     // raw 1
const Ratio half = 0.25_bps;        // raw 2'500
const Ratio cap = 5_bps;            // raw 50'000
```

Literals are parsed exactly at compile time. `_px` and `_qty` take up to 8 decimals, `_bps` up to 4;
more decimals, or a value out of range, is a compile error:

```text
error: static assertion failed: fastmm: _qty literal needs more than 8 decimals or is out of range
```

Digit separators (`1'000.5_px`) and exponents (`1e-8_qty`) are accepted. There is no Notional
literal; use `Notional::from_int(1000)` or `mul(price, qty)`.

## Parsing and formatting

| Function | Accepts | Use |
|---|---|---|
| `Fixed::from_decimal(s)` | `[+-]digits[.digits]`, up to 8 significant decimals | venue strings (strict: no exponent, no whitespace) |
| `Fixed::parse(s)` | the same plus an exponent: `2e-05`, `1.5E3` | configuration values |
| `p.to_decimal(buf)` | | exact, trailing zeros trimmed |
| `Fixed::from_double(d)`, `to_double()` | | startup and diagnostics only, never per event |
| `Ratio::from_bps(double)`, `r.to_bps()` | | startup and diagnostics only |

`parse` rejects a value only when more decimals remain after applying the exponent than the type
holds: `2e-05` is raw 2,000, `1.5e-8` is an error. TOML floats reach strategy parameters formatted
by fmt (`0.00002` becomes `2e-05`) and Python floats through `repr`
([Strategy API](strategy-api.md#parameters)).

## Quoting helpers

`include/fastmm/strategies/quoting.hpp`:

| Helper | Result |
|---|---|
| `mid(bid, ask)` | `(bid + ask) / 2`, truncated like `book.mid()` |
| `microprice(Level bid, Level ask)` | `(bid * ask_qty + ask * bid_qty) / (bid_qty + ask_qty)`, Int128, truncated; the mid when both sizes are zero |
| `spread_ratio(bid, ask)` | `(ask - bid) / mid` as a `Ratio` (`.to_bps()` for display); zero for a non-positive mid |
| `away_from(ref, side, dist)` | `ref - dist` for a bid, `ref + dist` for an ask |
| `inventory_allows(side, position, qty, limit)` | Buy: `position + qty <= limit`; Sell: `position - qty >= -limit`; a zero limit means no cap |
| `keep_passive(q, best_bid, best_ask, tick)` | shifts each side's whole ladder so level 0 sits at least one tick inside the touch, keeping the spacing; bids pushed to a non-positive price are removed |

On `DesiredQuotes`:

| Member | Effect |
|---|---|
| `q.bid(px, qty)`, `q.ask(px, qty)` | append the next level; a non-positive price or quantity, or a ninth level, is dropped and `false` returned |
| `q.uncross(tick)` | if the level-0 bid is at or above the level-0 ask, move that ask to bid + tick |

`inventory_allows` checks the result of the fill, not `|position|`: at a long limit the sell side
is still allowed, and so is a reducing trade from beyond the limit when it lands inside.

## Example

BasicMM's quoting function, from `include/fastmm/strategies/basic_mm.hpp`:

```cpp
const BasicMMParams& p = params();
DesiredQuotes q;
if (p.quote_qty.is_zero()) return q;
const std::int64_t inventory_units = position / p.quote_qty;  // signed, truncating
const Price half = mid * p.half_spread_bps;
const Price centre = mid - mid * (p.skew_bps_per_unit * inventory_units);
const Price step = inst.ticks(p.level_step_ticks);
const Qty qty = inst.round_qty(p.quote_qty);
const bool can_buy = inventory_allows(Side::Buy, position, p.quote_qty, p.max_inventory);
const bool can_sell = inventory_allows(Side::Sell, position, p.quote_qty, p.max_inventory);
for (int l = 0; l < p.levels; ++l) {
  if (can_buy) q.bid(inst.round_price(centre - half - step * l, Side::Buy), qty);
  if (can_sell) q.ask(inst.round_price(centre + half + step * l, Side::Sell), qty);
}
q.uncross(inst.tick);
```

## Limits

- `Ratio * k` is a plain int64 multiply: `skew * inventory_units` overflows past about 9.2e10
  units at 1 bp.
- A `Ratio` product with a value truncates once. `mid * a * b` truncates twice; write
  `mid * (a * b)` only when the precision of `a * b` (1e-8) is enough, otherwise scale in one step.
- Ranges are checked where values enter (instrument load, parameter parsing, literals), not on
  every operation.
