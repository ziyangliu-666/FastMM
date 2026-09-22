#include "fastmm/core/hot_array.hpp"

#include <sys/mman.h>

#include <cstdint>
#include <cstring>
#include <new>

namespace fastmm::detail {

namespace {
constexpr std::size_t kHugePage = std::size_t{2} << 20;
}  // namespace

void* hot_alloc(std::size_t bytes, std::size_t& mapped) {
  if (bytes == 0) bytes = 1;
  if (bytes < kHotArrayHugeMin) {
    mapped = 0;
    void* p = ::operator new(bytes, std::align_val_t{kCacheLine});
    std::memset(p, 0, bytes);
    return p;
  }
  // Over-map by one huge page so the array can start on a 2 MiB boundary, then trim.
  const std::size_t len = (bytes + kHugePage - 1) / kHugePage * kHugePage;
  void* raw =
      ::mmap(nullptr, len + kHugePage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (raw == MAP_FAILED) throw std::bad_alloc();
  auto* base = static_cast<std::byte*>(raw);
  const std::size_t head =
      (kHugePage - reinterpret_cast<std::uintptr_t>(base) % kHugePage) % kHugePage;
  if (head != 0) ::munmap(base, head);
  ::munmap(base + head + len, kHugePage - head);
  std::byte* p = base + head;
#ifdef MADV_HUGEPAGE
  static_cast<void>(::madvise(p, len, MADV_HUGEPAGE));
#endif
  std::memset(p, 0, len);  // fault every page in now (anonymous memory is zero already)
  mapped = len;
  return p;
}

void hot_free(void* p, std::size_t mapped) noexcept {
  if (p == nullptr) return;
  if (mapped != 0) {
    ::munmap(p, mapped);
  } else {
    ::operator delete(p, std::align_val_t{kCacheLine});
  }
}

}  // namespace fastmm::detail
