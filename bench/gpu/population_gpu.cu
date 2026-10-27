// population_gpu: the lanes of a population on a GPU, bit for bit against the CPU.
//
//   population_gpu TAPE LANES [--repeat R] [--block B]
//
// TAPE and LANES are what `fastmm-bench-population --tape-out P` writes (P.tape, P.lanes): the
// compiled shared market, and the lanes' kernel parameters followed by the CPU's outputs of
// lane_kernel.hpp for every (lane, instrument). Each GPU thread runs one (lane, instrument) through
// the same lane_kernel.hpp; a block holds lanes of one instrument, so its threads read the same
// records, which the block stages in shared memory chunk by chunk. Every output is compared with
// the CPU's, field by field. --repeat R runs the lanes R times over (R * lanes threads) to time a
// larger population.
#include "fastmm/backtest/lane_kernel.hpp"

#include <cuda_runtime.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace fastmm::lk;

#define CK(x)                                                                                 \
  do {                                                                                        \
    cudaError_t e_ = (x);                                                                     \
    if (e_ != cudaSuccess) {                                                                  \
      std::fprintf(stderr, "%s:%d %s: %s\n", __FILE__, __LINE__, #x, cudaGetErrorString(e_)); \
      std::exit(1);                                                                           \
    }                                                                                         \
  } while (0)

namespace {

template <class T>
std::vector<T> get_vec(std::FILE* f) {
  std::uint64_t n = 0;
  if (std::fread(&n, sizeof n, 1, f) != 1) std::exit(3);
  std::vector<T> v(n);
  if (n != 0 && std::fread(v.data(), sizeof(T), n, f) != n) std::exit(3);
  return v;
}

constexpr int kChunk = 128;  // records staged per block per round

__global__ void run_lanes(const Rec* recs,
                          const std::uint64_t* rec_off,
                          const Levels* levels,
                          const i64* adds,
                          std::uint64_t nadds,
                          const Inst* insts,
                          Shared shared,
                          const Params* params,
                          std::uint32_t lanes,
                          std::uint32_t ninst,
                          std::uint32_t repeat,
                          Out* out) {
  __shared__ Rec chunk[kChunk];
  // Units of one instrument are contiguous: unit = inst * (lanes * repeat) + copy * lanes + lane.
  const std::uint64_t per_inst = static_cast<std::uint64_t>(lanes) * repeat;
  const std::uint64_t unit = static_cast<std::uint64_t>(blockIdx.x) * blockDim.x + threadIdx.x;
  // The block's instrument (blockDim divides lanes * repeat).
  const std::uint32_t inst =
      static_cast<std::uint32_t>((static_cast<std::uint64_t>(blockIdx.x) * blockDim.x) / per_inst);
  const bool active = unit < per_inst * ninst;
  const std::uint32_t lane = static_cast<std::uint32_t>((unit % per_inst) % lanes);
  const std::uint32_t copy = static_cast<std::uint32_t>((unit % per_inst) / lanes);
  Out o;
  Lane k(params[active ? lane : 0], insts[inst], shared, levels, adds, nadds, &o);
  const std::uint64_t b = rec_off[inst];
  const std::uint64_t e = rec_off[inst + 1];
  for (std::uint64_t base = b; base < e; base += kChunk) {
    const std::uint64_t n = e - base < kChunk ? e - base : kChunk;
    __syncthreads();
    for (std::uint64_t j = threadIdx.x; j < n; j += blockDim.x) chunk[j] = recs[base + j];
    __syncthreads();
    if (active) {
      for (std::uint64_t j = 0; j < n; ++j) k.step(chunk[j]);
    }
  }
  if (!active) return;
  k.finish();
  out[(static_cast<std::uint64_t>(copy) * lanes + lane) * ninst + inst] = o;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s TAPE LANES [--repeat R] [--block B]\n", argv[0]);
    return 2;
  }
  std::uint32_t repeat = 1;
  int block = 128;
  for (int i = 3; i + 1 < argc; i += 2) {
    if (std::strcmp(argv[i], "--repeat") == 0)
      repeat = static_cast<std::uint32_t>(std::atoi(argv[i + 1]));
    if (std::strcmp(argv[i], "--block") == 0) block = std::atoi(argv[i + 1]);
  }
  // The tape (population.cpp Tape::save).
  std::FILE* f = std::fopen(argv[1], "rb");
  if (f == nullptr) return 3;
  std::uint64_t magic = 0;
  Shared shared{};
  if (std::fread(&magic, sizeof magic, 1, f) != 1 || std::fread(&shared, sizeof shared, 1, f) != 1)
    return 3;
  const std::vector<Inst> insts = get_vec<Inst>(f);
  const std::vector<Levels> levels = get_vec<Levels>(f);
  const std::vector<i64> adds = get_vec<i64>(f);
  std::vector<Rec> recs;
  std::vector<std::uint64_t> off{0};
  for (std::size_t i = 0; i < insts.size(); ++i) {
    const std::vector<Rec> r = get_vec<Rec>(f);
    recs.insert(recs.end(), r.begin(), r.end());
    off.push_back(recs.size());
  }
  std::fclose(f);
  // The lanes and the CPU's outputs.
  f = std::fopen(argv[2], "rb");
  if (f == nullptr) return 3;
  std::uint64_t nl = 0;
  if (std::fread(&nl, sizeof nl, 1, f) != 1) return 3;
  std::vector<Params> params(nl);
  if (std::fread(params.data(), sizeof(Params), nl, f) != nl) return 3;
  const std::uint32_t ninst = static_cast<std::uint32_t>(insts.size());
  std::vector<Out> want(nl * ninst);
  if (std::fread(want.data(), sizeof(Out), want.size(), f) != want.size()) return 3;
  std::fclose(f);
  const auto lanes = static_cast<std::uint32_t>(nl);
  if ((static_cast<std::uint64_t>(lanes) * repeat) % static_cast<std::uint64_t>(block) != 0) {
    std::fprintf(stderr, "lanes * repeat must be a multiple of the block size\n");
    return 2;
  }
  cudaDeviceProp prop{};
  CK(cudaGetDeviceProperties(&prop, 0));
  std::printf(
      "device %s, %d SMs; tape %zu records, %zu level tables, %zu arrivals; %u lanes x %u "
      "instruments x %u\n",
      prop.name,
      prop.multiProcessorCount,
      recs.size(),
      levels.size(),
      adds.size(),
      lanes,
      ninst,
      repeat);
  CK(cudaDeviceSetLimit(cudaLimitStackSize, 16 * 1024));

  Rec* d_recs = nullptr;
  std::uint64_t* d_off = nullptr;
  Levels* d_lv = nullptr;
  i64* d_adds = nullptr;
  Inst* d_inst = nullptr;
  Params* d_par = nullptr;
  Out* d_out = nullptr;
  const auto t_copy = std::chrono::steady_clock::now();
  CK(cudaMalloc(&d_recs, recs.size() * sizeof(Rec)));
  CK(cudaMalloc(&d_off, off.size() * sizeof(std::uint64_t)));
  CK(cudaMalloc(&d_lv, levels.size() * sizeof(Levels)));
  CK(cudaMalloc(&d_adds, adds.size() * sizeof(i64)));
  CK(cudaMalloc(&d_inst, insts.size() * sizeof(Inst)));
  CK(cudaMalloc(&d_par, params.size() * sizeof(Params)));
  const std::uint64_t units = static_cast<std::uint64_t>(lanes) * repeat * ninst;
  CK(cudaMalloc(&d_out, units * sizeof(Out)));
  CK(cudaMemcpy(d_recs, recs.data(), recs.size() * sizeof(Rec), cudaMemcpyHostToDevice));
  CK(cudaMemcpy(d_off, off.data(), off.size() * sizeof(std::uint64_t), cudaMemcpyHostToDevice));
  CK(cudaMemcpy(d_lv, levels.data(), levels.size() * sizeof(Levels), cudaMemcpyHostToDevice));
  CK(cudaMemcpy(d_adds, adds.data(), adds.size() * sizeof(i64), cudaMemcpyHostToDevice));
  CK(cudaMemcpy(d_inst, insts.data(), insts.size() * sizeof(Inst), cudaMemcpyHostToDevice));
  CK(cudaMemcpy(d_par, params.data(), params.size() * sizeof(Params), cudaMemcpyHostToDevice));
  CK(cudaMemset(d_out, 0, units * sizeof(Out)));
  CK(cudaDeviceSynchronize());
  const double copy_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t_copy).count();

  const auto blocks = static_cast<unsigned>((units + static_cast<std::uint64_t>(block) - 1) /
                                            static_cast<std::uint64_t>(block));
  cudaEvent_t a, b;
  CK(cudaEventCreate(&a));
  CK(cudaEventCreate(&b));
  CK(cudaEventRecord(a));
  run_lanes<<<blocks, block>>>(
      d_recs, d_off, d_lv, d_adds, adds.size(), d_inst, shared, d_par, lanes, ninst, repeat, d_out);
  CK(cudaGetLastError());
  CK(cudaEventRecord(b));
  CK(cudaEventSynchronize(b));
  float ms = 0;
  CK(cudaEventElapsedTime(&ms, a, b));
  std::vector<Out> got(units);
  CK(cudaMemcpy(got.data(), d_out, got.size() * sizeof(Out), cudaMemcpyDeviceToHost));

  std::size_t differ = 0;
  std::size_t first = got.size();
  for (std::size_t u = 0; u < got.size(); ++u) {
    if (std::memcmp(&got[u], &want[u % want.size()], sizeof(Out)) != 0) {
      if (first == got.size()) first = u;
      ++differ;
    }
  }
  std::uint64_t fills = 0;
  for (const Out& o : got) fills += o.fills;
  const double lane_days = static_cast<double>(lanes) * repeat;
  std::printf(
      "gpu: %.0f lanes (%llu threads) in %.3f s kernel, %.3f ms per lane-day, %.0f "
      "lane-days per second; upload %.2f s\n",
      lane_days,
      static_cast<unsigned long long>(units),
      ms / 1000.0,
      ms / lane_days,
      lane_days / (ms / 1000.0),
      copy_s);
  std::printf("check: %zu of %zu (lane, instrument) outputs differ from the CPU's; fills %llu\n",
              differ,
              got.size(),
              static_cast<unsigned long long>(fills));
  if (differ != 0) {
    const Out& g = got[first];
    const Out& w = want[first % want.size()];
    std::printf(
        "  first: unit %zu gpu fills %llu cash %lld err %u, cpu fills %llu cash %lld err %u\n",
        first,
        static_cast<unsigned long long>(g.fills),
        static_cast<long long>(g.cash),
        g.error,
        static_cast<unsigned long long>(w.fills),
        static_cast<long long>(w.cash),
        w.error);
  }
  return differ == 0 ? 0 : 1;
}
