#include "fastmm/core/arena.hpp"

#include <sys/mman.h>

#include <cstring>
#include <new>

namespace fastmm {

namespace {
constexpr std::size_t kHugePage = 2U << 20;
constexpr std::size_t round_up(std::size_t v, std::size_t a) noexcept {
  return (v + a - 1) / a * a;
}
}  // namespace

Arena::Arena(std::size_t bytes) {
  capacity_ = round_up(bytes == 0 ? 1 : bytes, kHugePage);
  // Map without MAP_POPULATE so the hugepage advice can take effect before faulting; then
  // touch every page ourselves. MAP_POPULATE would fault 4 KiB pages first.
  void* p = ::mmap(nullptr, capacity_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) throw std::bad_alloc();
  base_ = static_cast<std::byte*>(p);
#ifdef MADV_HUGEPAGE
  huge_ = ::madvise(base_, capacity_, MADV_HUGEPAGE) == 0;
#endif
  std::memset(base_, 0, capacity_);  // pre-fault (equivalent of MAP_POPULATE)
}

Arena::~Arena() {
  if (base_ != nullptr) ::munmap(base_, capacity_);
}

Arena::Arena(Arena&& o) noexcept
    : base_(o.base_), capacity_(o.capacity_), used_(o.used_), huge_(o.huge_) {
  o.base_ = nullptr;
  o.capacity_ = o.used_ = 0;
}

void* Arena::allocate(std::size_t bytes, std::size_t align) noexcept {
  FASTMM_ASSERT((align & (align - 1)) == 0);
  const std::size_t start = round_up(used_, align);
  if (start + bytes > capacity_ || start + bytes < start) return nullptr;
  used_ = start + bytes;
  return base_ + start;
}

}  // namespace fastmm
