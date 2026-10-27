# Venue connectors

A `[venues.<name>]` section picks a connector with `kind`:

| Venue | `kind` | Markets | Order entry | Test environment |
|---|---|---|---|---|
| [Binance Spot](venues/binance-spot.md) | `binance_spot`, `binance`, `sim` | spot | WebSocket API | Demo Mode, Spot testnet, [fastmm-sim-exchange](sim-exchange.md) |
| [Binance USDⓈ-M](venues/binance-usdm.md) | `binance_usdm` | perpetuals | WebSocket API | Demo Trading |
| [Bybit v5](venues/bybit.md) | `bybit`, `bybit_spot` | spot, linear perpetuals | WebSocket | testnet |
| [OKX v5](venues/okx.md) | `okx` | spot, USDT swaps | WebSocket | Demo trading |
| [Gate](venues/gate.md) | `gate_usdt`, `gate`, `gate_futures` | USDT perpetuals | WebSocket API | futures TestNet |
| [Hyperliquid](venues/hyperliquid.md) | `hyperliquid`, `hl` | perpetuals, HIP-3 dexes | WebSocket | testnet |
| [Lighter](venues/lighter.md) | `lighter`, `zklighter` | perpetuals | WebSocket | testnet |
| [Gemini](venues/gemini.md) | `gemini` | spot, perpetuals | WebSocket | sandbox |
| [Coinbase Advanced Trade](venues/coinbase.md#advanced-trade) | `coinbase_advanced` | spot | REST | none: production only |
| [Coinbase Exchange](venues/coinbase.md#exchange) | `coinbase_exchange` | spot | REST | sandbox |
| [Deribit](venues/deribit.md) | `deribit` | options, futures | WebSocket | testnet |
| [Alpaca](venues/alpaca.md) | `alpaca` | US stocks and ETFs | REST | paper account |
| [Nasdaq TotalView-ITCH](venues/nasdaq-itch.md) | `nasdaq_itch` | US stocks, market data | OUCH 5.0 to [fastmm-sim-itch](sim-itch.md) | fastmm-sim-itch |

What all connectors share, from threads to reconciliation, is on [Connector behaviour](venues/connector-behaviour.md). A project can register its own connector without changing FastMM ([Add a venue](../how-to/venues/add-a-venue.md)). Operating them: [Run in production](../how-to/operations/running-in-production.md).

## Capabilities

| Venue | Replace | Venue-side [dead man's switch](venues/connector-behaviour.md#dead-mans-switch) | [Fill replay](venues/connector-behaviour.md#execution-replay) | Balance stream | [Mark, funding](venues/connector-behaviour.md#mark-index-and-funding) |
|---|---|---|---|---|---|
| Binance Spot | amend or cancel-replace | none | yes | yes | |
| Binance USDⓈ-M | modify | countdown | yes | refetch | yes |
| Bybit | amend | on request | yes | yes | linear |
| OKX | amend | countdown | yes | yes | yes |
| Gate | amend | countdown | yes | refetch | yes |
| Hyperliquid | modify | scheduled | yes | no | funding |
| Lighter | modify | scheduled | no | no | no |
| Gemini | none | on disconnect | yes | yes | yes |
| Coinbase Advanced | none | none | yes | no | |
| Coinbase Exchange | none | none | yes | yes | |
| Deribit | edit | on disconnect | yes | yes | yes |
| Alpaca | new order | none | yes | no | |
| Nasdaq ITCH | OUCH replace | none | no | no | |

Binance Spot and Binance USDⓈ-M can run several accounts behind one venue ([account pools](venues/connector-behaviour.md#account-pools)). Each venue page ends with what is not handled and what is not yet confirmed against the venue.

## Configuration keys

These keys are parsed by FastMM for every venue: `kind`, `ws_url`, `ws_api_url`, `rest_url`, `api_key`, `api_secret`, `api_passphrase` ([secrets](configuration.md#general-rules)), `testnet`, `supports_replace`, `insecure_tls`, `ca_file`, `recv_window_ms`, `fees`.

Every other key in a `[venues.<name>]` section belongs to its connector, which declares and validates it. The tables under [Connectors](configuration.md#connectors) list them, one per `kind`. A key the connector does not own, or one of the wrong type, stops the session with its line.

Credentials that are not a plain key and secret:

| Venue | Credentials |
|---|---|
| Deribit | `api_key` is the client id, `api_secret` the client secret: `${FASTMM_DERIBIT_CLIENT_ID}`, `${FASTMM_DERIBIT_CLIENT_SECRET}` |
| OKX | `api_key`, `api_secret` and `api_passphrase` `${FASTMM_OKX_API_PASSPHRASE}` |
| Coinbase Advanced Trade | `api_key` is the CDP key name, `api_secret` its EC private key: `${COINBASE_API_KEY_NAME}`, `${COINBASE_API_PRIVATE_KEY}` |
| Coinbase Exchange | `api_key`, `api_secret` and `api_passphrase`: `${FASTMM_COINBASE_API_KEY}`, `${FASTMM_COINBASE_API_SECRET}`, `${FASTMM_COINBASE_API_PASSPHRASE}` |
| Hyperliquid | `api_key` is the account address, `api_secret` the signing key |
| Lighter | `api_key` is the account index, `api_secret` the API key's private key, `api_key_index` its slot |
| Nasdaq ITCH | none; OUCH logs in with `ouch_username` and `ouch_password` |

### Liveness

A market-data connection without traffic for `stale_ms` is reported `Stale`: the engine pulls the venue's quotes and clears its books, and the connector fetches a new snapshot when data returns. A connection silent for `dead_ms` is reconnected.

| Venue | `stale_ms` default | `dead_ms` at least |
|---|---|---|
| Binance Spot | 2000 ms | 45000 ms |
| Binance USDⓈ-M | 2000 ms | 45000 ms market data, 240000 ms other connections |
| Bybit, OKX | 2000 ms | 2 × `ping_interval_ms` + 5000 ms; 45000 ms by default |
| Gate | 2000 ms | 2 × `ping_interval_ms` + 5000 ms; 35000 ms by default |
| Gemini | 2000 ms | 2 × `ping_interval_ms` + 5000 ms; 30000 ms by default |
| Coinbase | 2000 ms | |
| Deribit | 10000 ms | three heartbeat intervals; 30000 ms by default |

Testnet BTCUSDT is often silent for more than 2 s, so the testnet and Demo configs set `stale_ms = 10000`, and `configs/deribit-testnet.toml` sets 15000.
