// round_trip: what handing one step to the GPU and taking its answer back costs: a launch, a
// kernel of eight threads, 64 bytes copied back and a synchronisation, timed over 20,000 steps.
//
//   nvcc -O2 -arch=native -o round_trip bench/gpu/round_trip.cu && ./round_trip
#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>

namespace {

__global__ void step(long* out, long v) {
  out[threadIdx.x] = v + threadIdx.x;
}

}  // namespace

int main() {
  long* d = nullptr;
  long h[8] = {};
  cudaMalloc(&d, sizeof h);
  cudaStream_t s;
  cudaStreamCreate(&s);
  const auto one = [&](long i) {
    step<<<1, 8, 0, s>>>(d, i);
    cudaMemcpyAsync(h, d, sizeof h, cudaMemcpyDeviceToHost, s);
    cudaStreamSynchronize(s);
  };
  for (long i = 0; i < 1000; ++i) one(i);
  constexpr long kSteps = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (long i = 0; i < kSteps; ++i) one(i);
  const double us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() /
      kSteps;
  std::printf("round trip: %.1f us a step (%s)\n", us, cudaGetErrorString(cudaGetLastError()));
  return h[7] == kSteps - 1 + 7 ? 0 : 1;
}
