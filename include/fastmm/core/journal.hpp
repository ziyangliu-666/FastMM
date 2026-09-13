#pragma once
// Journal `.fmj` (5.5, ADR-0010): every event the engine consumes, every outbound message
// and periodic latency samples, in consumption order, so a session can be replayed exactly.
//
//   file  := FileHeader (256 B) | Instrument[instrument_count] | Block* | Trailer?
//   block := BlockHeader (64 B, crc32c over payload) | messages (raw EventHeader-prefixed)
//
// Blocks are 1 MiB max; a message never straddles blocks. A trailer is an empty block with
// kBlockFlagTrailer; if it is missing the file was not closed cleanly and the reader
// discards any partial tail block (crc mismatch / short read).
//
// Split: JournalWriter (engine thread, assigns seq and copies into a MsgRing) and
// JournalFileWriter (background thread, ring -> mmap file). JournalReader mmaps a file.
#include "fastmm/core/config_macros.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/msg_ring.hpp"
#include "fastmm/core/result.hpp"
#include "fastmm/core/time.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <thread>

namespace fastmm {

inline constexpr std::uint32_t kJournalVersion = 1;
inline constexpr std::size_t kJournalBlockBytes = 1U << 20;
inline constexpr std::size_t kJournalExtentBytes = 64U << 20;
inline constexpr std::uint32_t kBlockFlagTrailer = 1U << 0;

struct JournalFileHeader {
  char magic[4];               // "FMJ1"
  std::uint32_t version;       // kJournalVersion
  std::uint32_t header_bytes;  // offset of the first block (header + instrument table)
  std::uint32_t instrument_count;
  std::uint64_t session_id;
  std::int64_t start_ts_ns;
  std::uint64_t tsc0;
  std::int64_t tsc_ns0;
  std::uint64_t tsc_ns_per_cycle_q32;
  std::uint64_t config_hash;
  std::uint64_t rng_seed;
  std::uint32_t message_version;
  std::uint32_t block_bytes;
  char strategy[32];
  std::uint8_t reserved[140];
  std::uint32_t crc32c;  // over the preceding 252 bytes
};
static_assert(sizeof(JournalFileHeader) == 256 && std::is_trivially_copyable_v<JournalFileHeader>);

struct JournalBlockHeader {
  char magic[4];           // "FMJB"
  std::uint32_t byte_len;  // payload bytes following this header
  std::uint64_t seq_first;
  std::uint64_t seq_last;
  std::uint32_t count;
  std::uint32_t crc32c;  // of the payload
  std::uint32_t flags;
  std::uint8_t reserved[28];
};
static_assert(sizeof(JournalBlockHeader) == 64);

enum class JournalError : std::uint8_t {
  Disabled,
  RingFull,
  OpenFailed,
  MapFailed,
  IoError,
  BadMagic,
  BadVersion,
  HeaderCorrupt,
  TooShort,
};
[[nodiscard]] constexpr std::string_view to_string(JournalError e) noexcept {
  switch (e) {
    case JournalError::Disabled:
      return "Disabled";
    case JournalError::RingFull:
      return "RingFull";
    case JournalError::OpenFailed:
      return "OpenFailed";
    case JournalError::MapFailed:
      return "MapFailed";
    case JournalError::IoError:
      return "IoError";
    case JournalError::BadMagic:
      return "BadMagic";
    case JournalError::BadVersion:
      return "BadVersion";
    case JournalError::HeaderCorrupt:
      return "HeaderCorrupt";
    case JournalError::TooShort:
      return "TooShort";
  }
  return "?";
}

// Engine-side: stamps the canonical sequence number and copies the message into the
// journal ring. Ring full is returned to the caller (the engine treats it as fatal per
// 5.5: kill switch + abort) and counted.
class JournalWriter {
 public:
  explicit JournalWriter(MsgRing* ring = nullptr) noexcept : ring_(ring) {}

  [[nodiscard]] bool enabled() const noexcept { return ring_ != nullptr; }
  [[nodiscard]] std::uint64_t next_seq() const noexcept { return next_seq_; }
  [[nodiscard]] std::uint64_t overflows() const noexcept { return overflows_; }
  [[nodiscard]] std::uint64_t recorded() const noexcept { return next_seq_ - 1; }

  // Copies `msg` (hdr.len bytes) with hdr.seq assigned. Returns the seq.
  Result<std::uint64_t, JournalError> record(const EventHeader& msg,
                                             std::uint8_t extra_flags = 0) noexcept {
    const std::uint64_t seq = next_seq_++;
    if (ring_ == nullptr) return seq;
    std::byte* p = ring_->try_reserve(msg.len);
    if (FASTMM_UNLIKELY(p == nullptr)) {
      ++overflows_;
      return fail(JournalError::RingFull);
    }
    std::memcpy(p, &msg, msg.len);
    auto* h = reinterpret_cast<EventHeader*>(p);
    h->seq = seq;
    h->flags = static_cast<std::uint8_t>(h->flags | extra_flags);
    ring_->commit();
    return seq;
  }
  Result<std::uint64_t, JournalError> record_outbound(const EventHeader& msg) noexcept {
    return record(msg, EventHeader::kOutbound);
  }

 private:
  MsgRing* ring_;
  std::uint64_t next_seq_ = 1;
  std::uint64_t overflows_ = 0;
};

struct JournalSessionInfo {
  std::uint64_t session_id = 0;
  Timestamp start_ts{};
  TscCalibration tsc{};
  std::uint64_t config_hash = 0;
  std::uint64_t rng_seed = 0;
  std::string_view strategy;
  const InstrumentTable* instruments = nullptr;
};

// Background side: drains the ring into 1 MiB blocks appended to an mmap'd file grown in
// 64 MiB extents; msync every 100 ms; stop() writes the trailer and truncates.
class JournalFileWriter {
 public:
  JournalFileWriter(MsgRing& ring, std::string path, const JournalSessionInfo& info);
  ~JournalFileWriter();
  JournalFileWriter(const JournalFileWriter&) = delete;
  JournalFileWriter& operator=(const JournalFileWriter&) = delete;

  [[nodiscard]] Result<void, JournalError> open_error() const noexcept {
    if (error_ != JournalError::Disabled) return fail(error_);
    return {};
  }
  [[nodiscard]] bool ok() const noexcept { return fd_ >= 0; }

  void start();  // spawns the drain thread
  void stop();   // joins, flushes, writes trailer, closes

  // Single-threaded driving (tests, backtest): drain the ring now; returns messages copied.
  std::size_t drain_once();
  void flush_block();  // writes the current partial block (if any)
  void sync() noexcept;

  [[nodiscard]] std::uint64_t blocks_written() const noexcept { return blocks_; }
  [[nodiscard]] std::uint64_t messages_written() const noexcept { return messages_; }
  [[nodiscard]] std::uint64_t bytes_written() const noexcept { return file_size_; }
  [[nodiscard]] const std::string& path() const noexcept { return path_; }

 private:
  void run();
  bool ensure_mapped(std::size_t bytes) noexcept;
  void append(const void* data, std::size_t len) noexcept;
  void write_trailer() noexcept;
  void close() noexcept;

  MsgRing& ring_;
  std::string path_;
  int fd_ = -1;
  std::byte* map_ = nullptr;
  std::size_t map_len_ = 0;
  std::size_t file_size_ = 0;                    // logical bytes written
  JournalError error_ = JournalError::Disabled;  // Disabled == no error

  std::unique_ptr<std::byte[]> block_;  // staging buffer (header + payload)
  std::uint32_t block_used_ = 0;        // payload bytes staged
  std::uint32_t block_count_ = 0;
  std::uint64_t block_seq_first_ = 0;
  std::uint64_t block_seq_last_ = 0;

  std::uint64_t blocks_ = 0;
  std::uint64_t messages_ = 0;
  Timestamp last_sync_{};
  Timestamp last_flush_{};

  std::thread thread_;
  std::atomic<bool> stop_{false};
  bool closed_ = false;
};

// Read-only mmap of a journal. Validates the header and every block's CRC; a truncated
// or corrupt tail block is discarded (truncated_tail() reports it).
class JournalReader {
 public:
  JournalReader() noexcept = default;
  ~JournalReader();
  JournalReader(const JournalReader&) = delete;
  JournalReader& operator=(const JournalReader&) = delete;
  JournalReader(JournalReader&& o) noexcept;
  JournalReader& operator=(JournalReader&& o) noexcept;

  [[nodiscard]] Result<void, JournalError> open(const std::string& path) noexcept;

  [[nodiscard]] const JournalFileHeader& header() const noexcept { return *header_; }
  [[nodiscard]] std::span<const Instrument> instruments() const noexcept {
    return {instruments_, header_->instrument_count};
  }
  [[nodiscard]] std::size_t block_count() const noexcept { return blocks_; }
  [[nodiscard]] std::size_t message_count() const noexcept { return messages_; }
  [[nodiscard]] bool truncated_tail() const noexcept { return truncated_tail_; }
  [[nodiscard]] bool has_trailer() const noexcept { return has_trailer_; }
  [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

  // Sequential cursor over messages. next() returns nullptr at end.
  void reset() noexcept {
    cur_block_ = first_block_;
    cur_off_ = 0;
  }
  [[nodiscard]] const EventHeader* next() noexcept;

  template <class F>
  void for_each(F&& f) noexcept {
    reset();
    while (const EventHeader* h = next()) f(h);
  }

 private:
  void validate() noexcept;  // walks blocks once, sets counts / truncated flag
  [[nodiscard]] const JournalBlockHeader* block_at(std::size_t off) const noexcept;

  const std::byte* map_ = nullptr;
  std::size_t map_len_ = 0;
  const JournalFileHeader* header_ = nullptr;
  const Instrument* instruments_ = nullptr;
  std::size_t first_block_ = 0;
  std::size_t valid_end_ = 0;  // byte offset one past the last valid block
  std::size_t blocks_ = 0;
  std::size_t messages_ = 0;
  std::uint64_t last_seq_ = 0;
  bool truncated_tail_ = false;
  bool has_trailer_ = false;
  std::size_t cur_block_ = 0;
  std::size_t cur_off_ = 0;  // payload offset within the current block
};

}  // namespace fastmm
