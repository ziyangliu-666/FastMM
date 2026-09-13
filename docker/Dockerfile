# Multi-stage: builder (gcc-13 + clang-18) -> slim runtime with only libssl3/zlib.
FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential g++-13 clang-18 lld-18 ninja-build git ca-certificates curl \
      libssl-dev zlib1g-dev python3 python3-pip python3-venv && \
    pip3 install --break-system-packages "cmake>=3.25" && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY . .
ENV CPM_SOURCE_CACHE=/root/.cache/CPM
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DFASTMM_BUILD_TESTS=OFF -DFASTMM_BUILD_BENCH=OFF \
      -DFASTMM_BUILD_EXAMPLES=OFF -DFASTMM_INSTALL=OFF && cmake --build build -j"$(nproc)"

FROM ubuntu:24.04 AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends libssl3 zlib1g ca-certificates && rm -rf /var/lib/apt/lists/*
WORKDIR /fastmm
COPY --from=builder /src/build/bin/ /fastmm/bin/
COPY --from=builder /src/configs/ /fastmm/configs/
COPY --from=builder /src/tests/fixtures/tls/ /fastmm/tls/
ENV PATH="/fastmm/bin:${PATH}"
ENTRYPOINT ["fastmm-live"]
CMD ["--config", "/fastmm/configs/sim-docker.toml"]
