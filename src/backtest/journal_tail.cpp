#include "fastmm/backtest/journal_tail.hpp"

#include "fastmm/core/crc32c.hpp"
#include "fastmm/core/messages.hpp"

#include <fmt/format.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <utility>

namespace fastmm::bt {

namespace {
constexpr char kBlockMagicText[4] = {'F', 'M', 'J', 'B'};
constexpr std::size_t kReleaseStep = std::size_t{16} << 20;

// All of `len` bytes at `off`, or false: the file is not that long yet.
bool read_at(int fd, void* to, std::size_t len, std::size_t off) noexcept {
  auto* p = static_cast<std::byte*>(to);
  while (len > 0) {
    const ssize_t n = ::pread(fd, p, len, static_cast<off_t>(off));
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    p += n;
    off += static_cast<std::size_t>(n);
    len -= static_cast<std::size_t>(n);
  }
  return true;
}
}  // namespace

JournalTail::JournalTail(std::string path, Duration trailer_grace)
    : base_(std::move(path)),
      path_(base_),
      trailer_grace_(trailer_grace),
      block_(sizeof(JournalBlockHeader) + kJournalBlockBytes) {}

JournalTail::~JournalTail() {
  close_fd();
}

void JournalTail::close_fd() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
}

bool JournalTail::open() {
  if (opened_) return true;
  std::error_code ec;
  if (!std::filesystem::exists(base_, ec)) return false;
  if (auto r = prologue_.open_prologue(base_); !r) {
    // The writer has made the file and not written its prologue yet.
    if (r.error() == JournalError::TooShort || r.error() == JournalError::BadMagic ||
        r.error() == JournalError::HeaderCorrupt)
      return false;
    throw std::runtime_error("journal " + base_ + ": " + prologue_.describe(r.error()));
  }
  session_id_ = prologue_.header().session_id;
  path_ = base_;
  if (!open_part()) return false;
  opened_ = true;
  last_block_ = steady_now();
  return true;
}

bool JournalTail::open_part() {
  const int fd = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  JournalFileHeader h{};
  if (!read_at(fd, &h, sizeof h, 0) || h.header_bytes < sizeof h ||
      crc32c(&h, offsetof(JournalFileHeader, crc32c)) != h.crc32c) {
    ::close(fd);
    return false;
  }
  if (h.session_id != session_id_) {
    ::close(fd);
    throw std::runtime_error(
        fmt::format("journal: {} is not a part of the session of {} (session {}, not {})",
                    path_,
                    base_,
                    h.session_id,
                    session_id_));
  }
  close_fd();
  fd_ = fd;
  off_ = h.header_bytes;
  released_ = 0;
  block_len_ = 0;
  at_ = 0;
  return true;
}

bool JournalTail::read_block() {
  JournalBlockHeader h{};
  if (!read_at(fd_, &h, sizeof h, off_) || std::memcmp(h.magic, kBlockMagicText, 4) != 0)
    return false;
  if ((h.flags & kBlockFlagTrailer) != 0) {
    in_trailer_ = true;
    closed_clean_ = true;
    trailer_at_ = steady_now();
    return false;
  }
  if (h.byte_len > kJournalBlockBytes) return false;
  if (!read_at(fd_, block_.data(), h.byte_len, off_ + sizeof h) ||
      crc32c(block_.data(), h.byte_len) != h.crc32c)
    return false;
  // A block holds whole records; one that does not is not written yet.
  for (std::uint32_t m = 0; m < h.byte_len;) {
    if (m + sizeof(EventHeader) > h.byte_len) return false;
    const auto* e = reinterpret_cast<const EventHeader*>(block_.data() + m);
    if (e->len < sizeof(EventHeader) || e->len % 64 != 0 || m + e->len > h.byte_len) return false;
    m += e->len;
  }
  if (last_seq_ != 0 && h.seq_first != last_seq_ + 1) ++seq_gaps_;
  last_seq_ = h.seq_last;
  block_len_ = h.byte_len;
  at_ = 0;
  off_ += sizeof h + h.byte_len;
  bytes_ += sizeof h + h.byte_len;
  last_block_ = steady_now();
  closed_clean_ = false;
  if (release_behind_ && off_ - released_ >= kReleaseStep) {
    // Pages the writer has not written back stay.
    ::posix_fadvise(fd_,
                    static_cast<off_t>(released_),
                    static_cast<off_t>(off_ - released_),
                    POSIX_FADV_DONTNEED);
    released_ = off_;
  }
  return true;
}

JournalTail::Step JournalTail::next(const EventHeader*& out) {
  if (!opened_ && !open()) return Step::Wait;
  for (;;) {
    if (at_ < block_len_) {
      const auto* h = reinterpret_cast<const EventHeader*>(block_.data() + at_);
      at_ += h->len;
      ++records_;
      if (h->type == EventType::EngineTime && h->len >= sizeof(EngineTimeMsg) &&
          msg_cast<EngineTimeMsg>(h).kind == EngineTimeMsg::Kind::Finish)
        finished_ = true;
      out = h;
      return Step::Record;
    }
    if (in_trailer_) {
      // A rotation opens the next part right after the trailer; the session's end opens none.
      const std::string next_path = JournalFileWriter::part_path(base_, part_ + 1);
      std::error_code ec;
      if (std::filesystem::exists(next_path, ec)) {
        std::string previous = std::move(path_);
        path_ = next_path;
        if (!open_part()) {
          path_ = std::move(previous);
          return Step::Wait;
        }
        ++part_;
        in_trailer_ = false;
        continue;
      }
      if (finished_ || gave_up_ || steady_now() - trailer_at_ >= trailer_grace_) return Step::End;
      return Step::Wait;
    }
    if (read_block() || in_trailer_) continue;
    return gave_up_ ? Step::End : Step::Wait;
  }
}

bool JournalTail::idle_for(Duration idle) const noexcept {
  return opened_ && steady_now() - last_block_ >= idle;
}

}  // namespace fastmm::bt
