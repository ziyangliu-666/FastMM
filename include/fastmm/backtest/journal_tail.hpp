#pragma once
// JournalTail: a journal read while it is being written, part by part, one record at a time.
//
// The writer (JournalFileWriter) reserves the file in extents of zeros and appends whole blocks
// with pwrite, so what lies past the last block reads as zeros, or as a block whose checksum does
// not hold yet. It shrinks the file when it closes a part, which is why the tail reads with pread
// and never maps it: a mapping touched past the new end is a SIGBUS. A block is handed on once its
// checksum holds; next() says Wait at the end of what is written. The session ends at the engine's
// EngineTime Finish record, at a trailer with no next part, or when the caller gives up on a writer
// that stopped writing (give_up()). The writer sees nothing of this reader.
#include "fastmm/core/journal.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fastmm::bt {

class JournalTail {
 public:
  enum class Step : std::uint8_t { Record, Wait, End };

  // `path`: the session's first part. Opens nothing yet. `trailer_grace`: how long a trailer waits
  // for a next part before it is the end of a session whose engine wrote no Finish record.
  explicit JournalTail(std::string path, Duration trailer_grace = seconds(30));
  ~JournalTail();
  JournalTail(const JournalTail&) = delete;
  JournalTail& operator=(const JournalTail&) = delete;

  // The first part's prologue, once there is one: false while it is missing or not written yet.
  // Throws std::runtime_error for a file of a format this build does not read.
  [[nodiscard]] bool open();
  [[nodiscard]] const JournalReader& prologue() const noexcept { return prologue_; }

  // The next record (valid until the next call) or why there is none.
  [[nodiscard]] Step next(const EventHeader*& out);

  // No block has come for `idle`.
  [[nodiscard]] bool idle_for(Duration idle) const noexcept;
  // The writer is taken to be gone: what is written is the session, and a block cut short is
  // dropped, as JournalReader drops it.
  void give_up() noexcept { gave_up_ = true; }

  [[nodiscard]] bool has_trailer() const noexcept { return closed_clean_; }
  [[nodiscard]] std::uint64_t records() const noexcept { return records_; }
  [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
  [[nodiscard]] std::uint64_t parts() const noexcept { return part_ + 1; }
  [[nodiscard]] std::uint64_t seq_gaps() const noexcept { return seq_gaps_; }
  // Drop the page cache behind the reader as it goes: a catch-up over days of journal does not push
  // the session's own pages out of memory.
  void set_release_behind(bool on) noexcept { release_behind_ = on; }

 private:
  [[nodiscard]] bool open_part();
  [[nodiscard]] bool read_block();
  void close_fd() noexcept;

  std::string base_;
  std::string path_;
  Duration trailer_grace_;
  JournalReader prologue_;
  bool opened_ = false;
  int fd_ = -1;
  std::uint64_t part_ = 0;
  std::size_t off_ = 0;       // file offset of the next block
  std::size_t released_ = 0;  // the page cache before this offset was dropped
  std::vector<std::byte> block_;
  std::uint32_t block_len_ = 0;  // payload bytes
  std::uint32_t at_ = 0;         // payload offset of the next record
  std::uint64_t last_seq_ = 0;
  std::uint64_t session_id_ = 0;
  std::uint64_t records_ = 0;
  std::uint64_t bytes_ = 0;
  std::uint64_t seq_gaps_ = 0;
  Timestamp last_block_{};  // steady clock
  Timestamp trailer_at_{};  // steady clock: a trailer was read, the next part not seen yet
  bool in_trailer_ = false;
  bool finished_ = false;      // the engine's Finish record has gone out
  bool closed_clean_ = false;  // the last part ended in a trailer
  bool gave_up_ = false;
  bool release_behind_ = false;
};

}  // namespace fastmm::bt
