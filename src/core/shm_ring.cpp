#include "fastmm/core/shm_ring.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bit>
#include <cerrno>
#include <cstring>
#include <new>
#include <utility>

namespace fastmm {

namespace {

std::string errno_text(const std::string& what, const std::string& path) {
  return what + " " + path + ": " + std::strerror(errno);
}

static_assert(sizeof(ShmRingHeader) + sizeof(ShmRingControl) + kDestructiveInterference <=
              ShmRing::kBufferOffset);

// The control block sits on its own lines after the header.
constexpr std::size_t kControlOffset = kDestructiveInterference;

}  // namespace

Result<ShmRing, std::string> ShmRing::create(const std::string& path, std::size_t capacity) {
  if (!std::has_single_bit(capacity) || capacity < kRingMsgGranule)
    return fail(std::string("shm ring capacity must be a power of two >= 64"));
  const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) return fail(errno_text("cannot create", path));
  const std::size_t len = kBufferOffset + capacity;
  if (::ftruncate(fd, static_cast<off_t>(len)) != 0) {
    std::string e = errno_text("ftruncate", path);
    ::close(fd);
    return fail(std::move(e));
  }
  void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return fail(errno_text("mmap", path));
  auto* base = static_cast<std::byte*>(p);
  ShmRing r;
  r.map_ = p;
  r.map_len_ = len;
  r.ctl_ = new (base + kControlOffset) ShmRingControl{};
  r.buf_ = base + kBufferOffset;
  r.mask_ = capacity - 1;
  // The header last: a process that opens the file early sees no magic and refuses it.
  auto* h = new (base) ShmRingHeader{};
  h->version = ShmRingHeader::kVersion;
  h->capacity = capacity;
  std::atomic_ref<std::uint64_t>(h->magic).store(ShmRingHeader::kMagic, std::memory_order_release);
  return r;
}

Result<ShmRing, std::string> ShmRing::open(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) return fail(errno_text("cannot open", path));
  struct stat st {};
  if (::fstat(fd, &st) != 0 || static_cast<std::size_t>(st.st_size) < kBufferOffset) {
    ::close(fd);
    return fail(path + ": not a shared ring (too short)");
  }
  const auto len = static_cast<std::size_t>(st.st_size);
  void* p = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return fail(errno_text("mmap", path));
  auto* base = static_cast<std::byte*>(p);
  auto* h = reinterpret_cast<ShmRingHeader*>(base);
  const std::uint64_t magic =
      std::atomic_ref<std::uint64_t>(h->magic).load(std::memory_order_acquire);
  if (magic != ShmRingHeader::kMagic || h->version != ShmRingHeader::kVersion ||
      !std::has_single_bit(h->capacity) || kBufferOffset + h->capacity != len) {
    ::munmap(p, len);
    return fail(path + ": not a shared ring of this version");
  }
  ShmRing r;
  r.map_ = p;
  r.map_len_ = len;
  r.ctl_ = reinterpret_cast<ShmRingControl*>(base + kControlOffset);
  r.buf_ = base + kBufferOffset;
  r.mask_ = h->capacity - 1;
  r.head_cache_ = r.ctl_->head.load(std::memory_order_acquire);
  r.tail_cache_ = r.ctl_->tail.load(std::memory_order_acquire);
  return r;
}

ShmRing::ShmRing(ShmRing&& o) noexcept
    : map_(std::exchange(o.map_, nullptr)),
      map_len_(std::exchange(o.map_len_, 0)),
      ctl_(std::exchange(o.ctl_, nullptr)),
      buf_(std::exchange(o.buf_, nullptr)),
      mask_(o.mask_),
      head_cache_(o.head_cache_),
      reserve_pos_(o.reserve_pos_),
      reserve_len_(o.reserve_len_),
      tail_cache_(o.tail_cache_) {}

ShmRing& ShmRing::operator=(ShmRing&& o) noexcept {
  if (this != &o) {
    unmap();
    map_ = std::exchange(o.map_, nullptr);
    map_len_ = std::exchange(o.map_len_, 0);
    ctl_ = std::exchange(o.ctl_, nullptr);
    buf_ = std::exchange(o.buf_, nullptr);
    mask_ = o.mask_;
    head_cache_ = o.head_cache_;
    reserve_pos_ = o.reserve_pos_;
    reserve_len_ = o.reserve_len_;
    tail_cache_ = o.tail_cache_;
  }
  return *this;
}

ShmRing::~ShmRing() {
  unmap();
}

void ShmRing::unmap() noexcept {
  if (map_ != nullptr) ::munmap(map_, map_len_);
  map_ = nullptr;
}

}  // namespace fastmm
