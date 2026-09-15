# Dependencies

| Dependency | Version | Scope | Why |
|---|---|---|---|
| fmt | 12.2.0 | core (public) | async logger formatting |
| toml++ | 3.4.0 | core (private) | configuration |
| simdjson | 4.6.11 | venues, sim server (private) | on-demand JSON parsing |
| doctest | 2.5.3 | tests | unit/property tests |
| Google Benchmark | 1.9.5 | bench | micro-benchmarks |
| pybind11 | 3.1.0 | python | bindings |
| OpenSSL | >= 3.0 (system); 3.5.8 linked statically in `fastmm-engine-live` wheels | net | TLS, HMAC, SHA |
| certifi | any | `fastmm-engine-live` (Python) | last CA bundle before OpenSSL's built-in paths |
| zlib | system | net | linked by `fastmm_net` |
| CPM.cmake | 0.43.1 | build | fetches and pins the dependencies above |

Pins are in `cmake/Dependencies.cmake`; `CPM_SOURCE_CACHE` (default `~/.cache/CPM`) makes repeat configures offline.
