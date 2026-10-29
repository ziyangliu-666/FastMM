# Trade US stocks on Alpaca

`configs/alpaca-paper.toml` runs `basic_mm` on SPY with an Alpaca paper account. The connector and the venue's rules: [Venue connectors](../../reference/venues/alpaca.md).

## 1. Keys

Open a paper account at <https://app.alpaca.markets> and generate a key under API Keys on the paper dashboard. Export the key id and the secret; the config reads them as `${ALPACA_API_KEY}` and `${ALPACA_API_SECRET}`:

```bash
export ALPACA_API_KEY=... ALPACA_API_SECRET=...
```

## 2. Check the connector on the account

```bash
cmake --preset release && cmake --build --preset release -j
./scripts/alpaca-paper-check.sh
```

Run it between 04:00 and 20:00 New York time. On SPY it prints each step's latency:

1. places a buy far below the market, replaces it and cancels it;
2. buys one share and sells it again;
3. reconnects;
4. starts a second connector over a resting order, as a restarted session does;
5. cancels everything.

## 3. Run a session

```bash
./build/release/bin/fastmm-live --config configs/alpaca-paper.toml
```

| New York time | Quotes |
|---|---|
| 08:00 to 09:30, 16:00 to 20:00 | rest and trade with `extended_hours = true` in `[venues.alpaca]`; IEX quotes from 08:00 |
| 09:30 to 16:00 | the regular session |

Orders go as `day` orders and expire at the end of the day. Ctrl-C cancels the session's orders and keeps the position, which the next start reads from the account ([Kill switch and shutdown](../operations/kill-switch-and-shutdown.md)).

From a flat position only the bid quotes, because the account refuses a short sale while a buy of the same stock is open. The first bid fill makes the account long and the ask follows. For two-sided quotes from the start, buy a few shares first and keep `max_inventory` at or below what you hold.

## 4. Settings to look at

| Key | Default | Change it when |
|---|---|---|
| `[engine] min_requote_interval_ms` | 2000 | Alpaca counts every request against 200 a minute per account |
| `[venues.alpaca] extended_hours` | false | quoting before 09:30 or after 16:00 |
| `[venues.alpaca] time_in_force` | `day` | orders should outlive the day (`gtc`) |
| `[venues.alpaca] ws_url` | the IEX stream | your Alpaca plan includes another feed (`v2/sip`) |

Paper fills follow the national best bid and offer with no queue position, so the paper account's PnL says nothing about spread capture.
