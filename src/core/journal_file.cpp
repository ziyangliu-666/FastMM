#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/journal.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace fastmm {

namespace {
constexpr char kFileMagic[4] = {'F', 'M', 'J', '1'};
constexpr char kBlockMagic[4] = {'F', 'M', 'J', 'B'};
constexpr Duration kSyncInterval = milliseconds(100);
}  // namespace

// ---- JournalFileWriter ----------------------------------------------------------------

JournalFileWriter::JournalFileWriter(MsgRing& ring,
                                     std::string path,
                                     const JournalSessionInfo& info)
    : ring_(ring),
      path_(std::move(path)),
      block_(new std::byte[sizeof(JournalBlockHeader) + kJournalBlockBytes]) {
  fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd_ < 0) {
    error_ = JournalError::OpenFailed;
    return;
  }
  const std::uint32_t ninst =
      info.instruments == nullptr ? 0 : static_cast<std::uint32_t>(info.instruments->size());
  JournalFileHeader h{};
  std::memcpy(h.magic, kFileMagic, 4);
  h.version = kJournalVersion;
  h.header_bytes =
      static_cast<std::uint32_t>(sizeof(JournalFileHeader) + ninst * sizeof(Instrument));
  h.instrument_count = ninst;
  h.session_id = info.session_id;
  h.start_ts_ns = info.start_ts.ns;
  h.tsc0 = info.tsc.tsc0;
  h.tsc_ns0 = info.tsc.ns0;
  h.tsc_ns_per_cycle_q32 = info.tsc.ns_per_cycle_q32;
  h.config_hash = info.config_hash;
  h.rng_seed = info.rng_seed;
  h.message_version = kMessageVersion;
  h.block_bytes = kJournalBlockBytes;
  const std::size_t n =
      info.strategy.size() < sizeof(h.strategy) - 1 ? info.strategy.size() : sizeof(h.strategy) - 1;
  std::memcpy(h.strategy, info.strategy.data(), n);
  h.crc32c = crc32c(&h, offsetof(JournalFileHeader, crc32c));
  if (!ensure_mapped(h.header_bytes)) return;
  append(&h, sizeof h);
  if (ninst > 0) append(info.instruments->data(), ninst * sizeof(Instrument));
  last_sync_ = last_flush_ = steady_now();
}

JournalFileWriter::~JournalFileWriter() {
  stop();
  close();
}

bool JournalFileWriter::ensure_mapped(std::size_t bytes) noexcept {
  if (fd_ < 0) return false;
  if (file_size_ + bytes <= map_len_) return true;
  std::size_t new_len = map_len_;
  while (file_size_ + bytes > new_len) new_len += kJournalExtentBytes;
  if (map_ != nullptr) {
    ::munmap(map_, map_len_);
    map_ = nullptr;
  }
  if (::ftruncate(fd_, static_cast<off_t>(new_len)) != 0) {
    error_ = JournalError::IoError;
    return false;
  }
  void* p = ::mmap(nullptr, new_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    error_ = JournalError::MapFailed;
    map_len_ = 0;
    return false;
  }
  map_ = static_cast<std::byte*>(p);
  map_len_ = new_len;
  return true;
}

void JournalFileWriter::append(const void* data, std::size_t len) noexcept {
  if (!ensure_mapped(len)) return;
  std::memcpy(map_ + file_size_, data, len);
  file_size_ += len;
}

void JournalFileWriter::flush_block() {
  if (block_count_ == 0) return;
  auto* bh = reinterpret_cast<JournalBlockHeader*>(block_.get());
  *bh = JournalBlockHeader{};
  std::memcpy(bh->magic, kBlockMagic, 4);
  bh->byte_len = block_used_;
  bh->seq_first = block_seq_first_;
  bh->seq_last = block_seq_last_;
  bh->count = block_count_;
  bh->crc32c = crc32c(block_.get() + sizeof(JournalBlockHeader), block_used_);
  append(block_.get(), sizeof(JournalBlockHeader) + block_used_);
  ++blocks_;
  block_used_ = 0;
  block_count_ = 0;
  last_flush_ = steady_now();
}

std::size_t JournalFileWriter::drain_once() {
  std::size_t n = 0;
  while (const std::byte* p = ring_.try_peek()) {
    const auto* h = reinterpret_cast<const EventHeader*>(p);
    const std::uint32_t len = h->len;
    if (block_used_ + len > kJournalBlockBytes) flush_block();
    if (block_count_ == 0) block_seq_first_ = h->seq;
    block_seq_last_ = h->seq;
    std::memcpy(block_.get() + sizeof(JournalBlockHeader) + block_used_, p, len);
    block_used_ += len;
    ++block_count_;
    ++messages_;
    ++n;
    ring_.release();
  }
  return n;
}

void JournalFileWriter::sync() noexcept {
  if (map_ != nullptr) ::msync(map_, file_size_, MS_ASYNC);
  last_sync_ = steady_now();
}

void JournalFileWriter::run() {
  int idle = 0;
  while (!stop_.load(std::memory_order_acquire)) {
    const std::size_t n = drain_once();
    const Timestamp now = steady_now();
    if (block_count_ > 0 && now - last_flush_ >= kSyncInterval) flush_block();
    if (now - last_sync_ >= kSyncInterval) sync();
    if (n == 0) {
      if (++idle > 200) {
        timespec ts{0, 50'000};
        nanosleep(&ts, nullptr);
      } else {
        __builtin_ia32_pause();
      }
    } else {
      idle = 0;
    }
  }
  drain_once();
  flush_block();
}

void JournalFileWriter::start() {
  if (fd_ < 0 || thread_.joinable()) return;
  stop_.store(false);
  thread_ = std::thread([this] { run(); });
}

void JournalFileWriter::stop() {
  if (thread_.joinable()) {
    stop_.store(true, std::memory_order_release);
    thread_.join();
  } else {
    drain_once();
    flush_block();
  }
  if (!closed_) {
    write_trailer();
    close();
  }
}

void JournalFileWriter::write_trailer() noexcept {
  if (fd_ < 0) return;
  JournalBlockHeader t{};
  std::memcpy(t.magic, kBlockMagic, 4);
  t.flags = kBlockFlagTrailer;
  t.seq_first = t.seq_last = block_seq_last_;
  t.crc32c = crc32c(nullptr, 0);
  append(&t, sizeof t);
}

void JournalFileWriter::close() noexcept {
  if (closed_) return;
  closed_ = true;
  if (map_ != nullptr) {
    ::msync(map_, file_size_, MS_SYNC);
    ::munmap(map_, map_len_);
    map_ = nullptr;
  }
  if (fd_ >= 0) {
    if (::ftruncate(fd_, static_cast<off_t>(file_size_)) != 0) error_ = JournalError::IoError;
    ::close(fd_);
    fd_ = -1;
  }
}

// ---- JournalReader ----------------------------------------------------------------------

JournalReader::~JournalReader() {
  if (map_ != nullptr) ::munmap(const_cast<std::byte*>(map_), map_len_);
}

JournalReader::JournalReader(JournalReader&& o) noexcept {
  *this = std::move(o);
}

JournalReader& JournalReader::operator=(JournalReader&& o) noexcept {
  if (this != &o) {
    if (map_ != nullptr) ::munmap(const_cast<std::byte*>(map_), map_len_);
    std::memcpy(static_cast<void*>(this), static_cast<const void*>(&o), sizeof(JournalReader));
    o.map_ = nullptr;
    o.map_len_ = 0;
  }
  return *this;
}

Result<void, JournalError> JournalReader::open(const std::string& path) noexcept {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return fail(JournalError::OpenFailed);
  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    ::close(fd);
    return fail(JournalError::IoError);
  }
  const auto len = static_cast<std::size_t>(st.st_size);
  if (len < sizeof(JournalFileHeader)) {
    ::close(fd);
    return fail(JournalError::TooShort);
  }
  void* p = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) return fail(JournalError::MapFailed);
  map_ = static_cast<const std::byte*>(p);
  map_len_ = len;
  header_ = reinterpret_cast<const JournalFileHeader*>(map_);
  if (std::memcmp(header_->magic, kFileMagic, 4) != 0) return fail(JournalError::BadMagic);
  if (header_->version != kJournalVersion) return fail(JournalError::BadVersion);
  if (crc32c(header_, offsetof(JournalFileHeader, crc32c)) != header_->crc32c)
    return fail(JournalError::HeaderCorrupt);
  if (header_->header_bytes > len || header_->header_bytes < sizeof(JournalFileHeader) ||
      header_->header_bytes !=
          sizeof(JournalFileHeader) + header_->instrument_count * sizeof(Instrument)) {
    return fail(JournalError::HeaderCorrupt);
  }
  instruments_ = reinterpret_cast<const Instrument*>(map_ + sizeof(JournalFileHeader));
  first_block_ = header_->header_bytes;
  validate();
  reset();
  return {};
}

const JournalBlockHeader* JournalReader::block_at(std::size_t off) const noexcept {
  if (off + sizeof(JournalBlockHeader) > map_len_) return nullptr;
  const auto* b = reinterpret_cast<const JournalBlockHeader*>(map_ + off);
  if (std::memcmp(b->magic, kBlockMagic, 4) != 0) return nullptr;
  if (off + sizeof(JournalBlockHeader) + b->byte_len > map_len_) return nullptr;
  return b;
}

void JournalReader::validate() noexcept {
  std::size_t off = first_block_;
  valid_end_ = off;
  while (off < map_len_) {
    const JournalBlockHeader* b = block_at(off);
    if (b == nullptr) {
      truncated_tail_ = true;
      break;
    }
    const std::byte* payload = map_ + off + sizeof(JournalBlockHeader);
    if (crc32c(payload, b->byte_len) != b->crc32c) {
      truncated_tail_ = true;
      break;
    }
    if ((b->flags & kBlockFlagTrailer) != 0) {
      has_trailer_ = true;
      valid_end_ = off;  // the trailer itself carries no messages
      break;
    }
    // Walk messages to make sure lengths are sane.
    std::size_t m = 0;
    bool sane = true;
    while (m < b->byte_len) {
      if (m + sizeof(EventHeader) > b->byte_len) {
        sane = false;
        break;
      }
      const auto* h = reinterpret_cast<const EventHeader*>(payload + m);
      if (h->len < sizeof(EventHeader) || h->len % 64 != 0 || m + h->len > b->byte_len) {
        sane = false;
        break;
      }
      m += h->len;
    }
    if (!sane) {
      truncated_tail_ = true;
      break;
    }
    ++blocks_;
    messages_ += b->count;
    last_seq_ = b->seq_last;
    off += sizeof(JournalBlockHeader) + b->byte_len;
    valid_end_ = off;
  }
}

const EventHeader* JournalReader::next() noexcept {
  while (cur_block_ < valid_end_) {
    const auto* b = reinterpret_cast<const JournalBlockHeader*>(map_ + cur_block_);
    if (cur_off_ < b->byte_len) {
      const auto* h = reinterpret_cast<const EventHeader*>(map_ + cur_block_ +
                                                           sizeof(JournalBlockHeader) + cur_off_);
      cur_off_ += h->len;
      return h;
    }
    cur_block_ += sizeof(JournalBlockHeader) + b->byte_len;
    cur_off_ = 0;
  }
  return nullptr;
}

}  // namespace fastmm
