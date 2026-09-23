# Design records

One record per decision, written when it was taken and not edited afterwards except by an amendment. They explain why the code is the way it is; they are not a description of what the code does today. For that, read [Architecture](../explanation/architecture.md) and the [reference](../README.md#reference).

| # | Decision | Status |
|---|---|---|
| [0001](0001-fixed-point-int64-price-qty.md) | Fixed-point int64 for `Price` and `Qty` | accepted |
| [0002](0002-exceptions-off-hot-path.md) | Exceptions and RTTI stay enabled, but off the hot path | accepted |
| [0003](0003-pragma-once.md) | `#pragma once` instead of include guards | accepted |
| [0004](0004-doctest-over-gtest.md) | doctest over GoogleTest | accepted |
| [0005](0005-cpm-over-submodules-conan-vcpkg.md) | CPM.cmake over submodules, Conan or vcpkg | accepted |
| [0006](0006-static-libs-lto.md) | Static libraries with LTO | accepted |
| [0007](0007-hand-written-net-stack.md) | A hand-written network stack over Boost.Asio and libwebsockets | accepted |
| [0008](0008-sim-exchange-speaks-binance.md) | The simulated exchange speaks the Binance protocol | accepted |
| [0009](0009-crtp-strategies-over-virtual.md) | CRTP strategies over virtual interfaces | accepted |
| [0010](0010-fmj-journal-format.md) | The `.fmj` journal format | accepted; amended by format versions 2 and 3 |
| [0011](0011-scikit-build-core.md) | scikit-build-core for Python packaging | accepted |
| [0012](0012-strategy-developer-experience.md) | Strategy developer experience | accepted; section 7 superseded by 0013 |
| [0013](0013-python-strategies-live.md) | Python strategies in live trading | accepted |
| [0014](0014-us-equities.md) | US equities | accepted, not implemented: the page is a plan, not the code |
| [0015](0015-multicast-market-data.md) | UDP multicast market data and kernel-bypass receive | accepted; amends 0014's market-data scope |
| [0016](0016-storage-backends.md) | Pluggable storage backends, SQLite first | accepted |
