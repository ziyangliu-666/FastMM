# Options

Options use the same engine, OMS, risk checks and QuoteManager as other instruments. Deribit
(`kind = "deribit"`, see [venues.md](venues.md)) is the only connector that emits option market
data.

## Instruments

An option is an `Instrument` with `asset_class = Option`, `option_type` (call or put), `strike` and
`expiry_ns`. `contract_multiplier` is the number of underlying units per contract. The `kInverse`
flag marks coin-quoted contracts, where the price and the PnL are in the base coin. Deribit fills
all of these from `public/get_instruments`:

| Deribit field | Instrument |
|---|---|
| `kind` option / future + `settlement_period` perpetual / spot | `asset_class` Option / Perpetual or Future / Spot |
| `option_type`, `strike`, `expiration_timestamp` | `option_type`, `strike`, `expiry_ns` |
| `contract_size` | `contract_multiplier` (1 BTC for BTC options, 10 USD for BTC-PERPETUAL) |
| `min_trade_amount / contract_size` | `lot` and `min_qty` (quantities are contracts) |
| `tick_size`, `tick_size_steps` | `tick`; the step grid is applied by the order encoder |
| `instrument_type` reversed | `kInverse` |

Deribit BTC options are inverse. They are quoted in BTC per 1 BTC of underlying (a mark of
`0.0069` is 0.0069 BTC), sized in BTC, and above a price of 0.005 their tick grows from 0.0001 to
0.0005.

The Deribit connector sends `contracts` rather than
`amount`, and converts book, trade and fill amounts back to contracts. Position PnL is
`(price − avg) × qty × multiplier`, which is the BTC PnL for inverse options. The engine's generic
notional risk limits are not inverse-aware, so size `max_order_notional` in the option's price unit.

## OptionTicker event

`EventType::OptionTicker` / `OptionTickerMsg` (192 bytes, `core/messages.hpp`) carries one
option's venue view:

| field | unit |
|---|---|
| `mark_price` | instrument price unit (BTC for Deribit inverse options) |
| `underlying_price` | quote currency: the forward the venue prices the option on |
| `index_price` | quote currency |
| `mark_iv`, `bid_iv`, `ask_iv` | annualised decimal (0.312 = 31.2 %); NaN if absent |
| `delta`, `gamma`, `vega`, `theta`, `rho` | as the venue reports them |
| `interest_rate` | annualised decimal |

The engine journals the event and passes it to the optional strategy hook
`on_option_ticker(ctx, id, msg)`, for instruments in the table only. `tools/journal_dump.py` prints it. The Deribit connector emits it for
every `ticker.{instrument}.{interval}` notification of an option, next to a `BookTicker`.

Deribit's greeks were checked against a recorded testnet ticker (`tests/core/black76_test.cpp`).
They are Black-76 with r = `interest_rate` (0 on the testnet), in USD: delta per unit of
underlying, gamma per USD, vega per vol point (Black-76 vega / 100) and theta per day (/ 365).
Rho is the spot-model `K T N(d2) / 100`, not the Black-76 rho.

## Black-76 library

`include/fastmm/core/options/black76.hpp`, namespace `fastmm::options`. Header-only, `noexcept`,
no allocation:

* `black76(cp, F, K, T, sigma, r)` returns price, delta (dV/dF), gamma, vega (per 1.00 of vol),
  theta (per year of calendar time) and rho (dV/dr = −T V).
* `implied_vol(cp, price, F, K, T, r, tol, max_iterations)`: Newton steps inside a
  [0.0001, 10] bracket, falling back to bisection whenever Newton leaves the bracket. The result
  status is `Ok`, `AtIntrinsic` (no time value to invert), `OutOfBounds` (below intrinsic, or above
  what 1000 % vol can produce), `BadInput` or `NoConvergence` (iteration cap hit; the bracket
  midpoint is returned).
* `year_fraction(expiry_ns, now_ns)` is ACT/365. `to_coin_price(V, F)` gives `V / F`, and
  `coin_delta(delta, price_coin)` gives `delta − price_coin`, the delta of a coin-quoted option
  measured in coins.

Tests use Haug's Black-76 example (F = K = 19, T = 0.75, r = 10 %, σ = 28 % → 1.7011), reference
values from an independent implementation, finite differences for every greek, implied-vol round
trips across moneyness, vol and tenor, and the recorded Deribit ticker.

## OptionsMM strategy

`include/fastmm/strategies/options_mm.hpp`, registered as `options_mm` for Sim, Replay and Live.
For each option it prices on every `OptionTicker`. With `use_venue_iv = false` it also reprices on
every book update.

```
F      = ticker underlying_price,  r = ticker interest_rate,  T = (expiry − now) / 365 d
sigma  = venue mark_iv, or the own EWMA of book-mid implied vols (half-life iv_halflife_s)
theo   = Black-76 price, / F for inverse (coin-quoted) options
vega_px= Black-76 vega / 100 in the same price unit
half   = max(half_spread_vol × vega_px, min_half_spread_ticks × tick)
         × (1 + vega_widen × min(1, |portfolio vega| / max_vega))
r_px   = theo − delta_skew_ticks × tick × (portfolio delta / max_delta) × option delta
              − inventory_skew_ticks × tick × (position / quote_qty)
bid    = r_px − half  (rounded down),  ask = r_px + half  (rounded up), kept inside the touch
```

Portfolio greeks are summed over the positions of every instrument in the context:

* options: `qty × contract_multiplier × delta` (for inverse options, delta minus the coin premium
  when `premium_adjusted_delta`), and `qty × contract_multiplier × vega / 100` in USD per vol point;
* futures, perpetuals and spot: `qty × contract_multiplier`, divided by the book mid for inverse
  contracts (a 10 USD inverse contract holds 10 / F BTC).

A side is not quoted when a fill of `quote_qty` would push |portfolio delta| above `max_delta`,
|portfolio vega| above `max_vega`, or |position| above `max_position`. A fill that reduces the
exposure is allowed. A fill requotes every option.

An option is quoted only while its book is two-sided, because the stale-market-data check rejects
quotes for an instrument without a valid book and the strategy does not see those rejects. Tickers
keep updating the option's greeks meanwhile; the first valid book update places its quotes.

`OptionTicker` counts as market data for journal sources (`JournalSource`), so a journal recorded
with `fastmm-live` against Deribit can drive a Sim backtest or a replay of `options_mm`.

### Parameters

| name | default | meaning |
|---|---|---|
| `use_venue_iv` | true | price with the venue mark IV; false: own EWMA of book-mid IVs |
| `iv_halflife_s` | 30 | half-life of the own IV EWMA, seconds |
| `half_spread_vol` | 1.0 | half spread in vol points (× option vega) |
| `min_half_spread_ticks` | 1 | floor for the half spread |
| `quote_qty` | 1.0 | contracts per side |
| `max_position` | 10 | per-option \|position\| cap, contracts (0 = none) |
| `max_delta` | 5 | portfolio \|delta\| limit, underlying units (0 = no limit and no delta skew) |
| `max_vega` | 1000 | portfolio \|vega\| limit, quote currency per vol point (0 = none) |
| `delta_skew_ticks` | 5 | reservation shift in ticks for a delta-1 option at `max_delta` |
| `inventory_skew_ticks` | 1 | reservation shift in ticks per `quote_qty` of the option's position |
| `vega_widen` | 1.0 | extra half spread (fraction) at `max_vega` |
| `min_expiry_s` | 3600 | no quotes for options expiring sooner |
| `requote_threshold_ticks` | 1 | ignore theo moves smaller than this |
| `premium_adjusted_delta` | true | inverse options: delta minus the coin premium |
| `pull_on_stale_ms` | 5000 | pull an option's quotes when its ticker is older (0 = never) |

`configs/deribit-testnet.toml` runs OptionsMM on three BTC options and BTC-PERPETUAL with
testnet-sized limits. Option names embed their expiry, so replace the `[[instruments]]` once they
expire: `load_reference_data` refuses an expired symbol.

### Limits and caveats

* The model is Black-76 on the venue's underlying price, with no smile or term-structure model of
  its own. The venue mark IV (or the smoothed mid IV) is used per strike as given.
* Portfolio greeks use the last pricing of each option. An option whose ticker has not arrived yet
  contributes no greeks.
* Delta hedging with futures is not automated. Futures positions only enter the delta used for
  skews and limits.
* OptionsMM evaluates its model in `double` and rounds the result onto the tick grid.
