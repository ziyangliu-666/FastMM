# Trade US stocks on Alpaca

`configs/alpaca-paper.toml` runs `basic_mm` on SPY with an Alpaca paper account: market data from the free IEX stream, orders over Alpaca's REST API, order states and fills from its `trade_updates` stream. What the connector does and the venue's rules: [Venue connectors](../../reference/venues.md#alpaca-us-stocks-alpaca).

## 1. Keys

Open a paper account at <https://app.alpaca.markets>, then under API Keys on the paper dashboard generate a key. Export the key id and the secret:

```bash
export ALPACA_API_KEY=... ALPACA_API_SECRET=...
```

The config reads them as `${ALPACA_API_KEY}` and `${ALPACA_API_SECRET}`.

## 2. Check the connector on the account

```bash
cmake --preset release && cmake --build --preset release -j
./scripts/alpaca-paper-check.sh
```

On SPY it places a buy far below the market, replaces and cancels it, buys one share and sells it again, reconnects, starts a second connector over a resting order the way a restarted session does, and cancels everything. Each step prints its latency. It needs the extended session or the regular one, 04:00 to 20:00 New York time.

## 3. Run a session

```bash
./build/release/bin/fastmm-live --config configs/alpaca-paper.toml
```

The regular session is 09:30 to 16:00 New York time. IEX quotes from 08:00; with `extended_hours = true` in `[venues.alpaca]` the quotes rest and trade from then until 20:00. Orders go as `day` orders and expire at the end of the day. Stop with Ctrl-C: the session cancels its orders and keeps the position, which the next start reads from the account ([Kill switch and shutdown](../operations/kill-switch-and-shutdown.md)).

The account refuses a short sale while a buy of the same stock is open, so from a flat position only one side quotes: the first fill of the bid makes the account long and the ask follows. Buy a few shares first for two-sided quoting from the start, and keep `max_inventory` at or below what you hold.

## 4. Settings to look at

| Key | Default | Change it when |
|---|---|---|
| `[engine] min_requote_interval_ms` | 2000 | Alpaca counts every request against 200 a minute per account |
| `[venues.alpaca] extended_hours` | false | quoting before 09:30 or after 16:00 |
| `[venues.alpaca] time_in_force` | `day` | orders should outlive the day (`gtc`) |
| `[venues.alpaca] ws_url` | the IEX stream | your Alpaca plan includes another feed (`v2/sip`) |

Paper fills follow the national best bid and offer with no queue position, so the paper account's PnL says nothing about spread capture.
