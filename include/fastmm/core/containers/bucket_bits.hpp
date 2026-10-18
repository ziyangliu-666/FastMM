#pragma once
// BucketBits<N, B>: for each of B buckets, which of the slots [0, N) of a Pool belong to it, so
// that the slots of one bucket (one instrument's orders) are visited in ascending order without
// scanning the others'. A bit per slot and a bit per non-empty 64-slot word, allocated once;
// set/clear and the visit do no allocation.
#include <cstddef>
#include <cstdint>
#include <memory>

namespace fastmm {

template <std::size_t N, std::size_t B>
class BucketBits {
  static constexpr std::size_t kWords = (N + 63) / 64;
  static_assert(kWords <= 64, "one summary word per bucket");
  static_assert(B > 0 && (B & (B - 1)) == 0, "a power of two of buckets");

 public:
  BucketBits()
      : bits_(std::make_unique<std::uint64_t[]>(B * kWords)),
        words_(std::make_unique<std::uint64_t[]>(B)) {}

  // A key's bucket: its low bits (instrument ids are dense from 0).
  [[nodiscard]] static constexpr std::size_t bucket(std::uint64_t key) noexcept {
    return key & (B - 1);
  }
  void set(std::size_t b, std::uint32_t slot) noexcept {
    bits_[(b * kWords) + (slot / 64)] |= 1ULL << (slot % 64);
    words_[b] |= 1ULL << (slot / 64);
  }
  void clear(std::size_t b, std::uint32_t slot) noexcept {
    std::uint64_t& w = bits_[(b * kWords) + (slot / 64)];
    w &= ~(1ULL << (slot % 64));
    if (w == 0) words_[b] &= ~(1ULL << (slot / 64));
  }
  // F(std::uint32_t slot) for each slot of bucket b, ascending. F may clear the slot it is given;
  // a slot set during the visit in a word that had none of the bucket's is not visited.
  template <class F>
  void for_each(std::size_t b, F&& f) const noexcept {
    const std::uint64_t* const bits = &bits_[b * kWords];
    std::uint64_t words = words_[b];
    while (words != 0) {
      const auto w = static_cast<std::uint32_t>(__builtin_ctzll(words));
      words &= words - 1;
      std::uint64_t m = bits[w];
      while (m != 0) {
        const auto slot = (w * 64) + static_cast<std::uint32_t>(__builtin_ctzll(m));
        m &= m - 1;
        f(slot);
      }
    }
  }

 private:
  std::unique_ptr<std::uint64_t[]> bits_;
  std::unique_ptr<std::uint64_t[]> words_;
};

}  // namespace fastmm
