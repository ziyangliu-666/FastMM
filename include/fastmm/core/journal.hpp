#pragma once
// Journal `.fmj` (5.5, ADR-0010): every event the engine consumes, every outbound message
// and periodic latency samples, in consumption order, so a session can be replayed exactly.
//
//   file  := FileHeader (256 B) | Instrument[instrument_count] | Config? | Block* | Trailer?
//   block := BlockHeader (64 B, crc32c over payload) | messages (raw EventHeader-prefixed)
//
// Format v2 (readers accept v1 and v2) adds:
//   * the engine clock of every consumed event: records flagged kEngineTime carry a signed int32
//     ns delta in EventHeader::reserved0 from the previous one; EngineTimeMsg records carry the
//     absolute value (engine start, finish, and any delta that does not fit);
//   * session settings in the header (session epoch, quoting enabled, per-venue cancel-replace)
//     and the effective configuration as TOML (secrets omitted), zero-padded to 64 bytes after
//     the instrument table;
//   * kDropped on outbound copies the transport did not accept.
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

inline constexpr std::uint32_t kJournalVersion = 2;
inline constexpr std::uint32_t kJournalMinVersion = 1;  // oldest version JournalReader opens
inline constexpr std::size_t kJournalBlockBytes = 1U << 20;
inline constexpr std::size_t kJournalExtentBytes = 64U << 20;
inline constexpr std::uint32_t kBlockFlagTrailer = 1U << 0;
inline constexpr std::uint8_t kHeaderSession = 1U << 0;  // v2 session settings are valid

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
  // ---- v2 (zero in v1 files) ----
  std::uint16_t session_epoch;   // client order id epoch (kHeaderSession)
  std::uint8_t quoting_enabled;  // 0 for a dry run (kHeaderSession)
  std::uint8_t header_flags;     // kHeaderSession
  std::uint32_t config_bytes;    // effective config TOML after the instrument table (0 = none)
  std::uint64_t replace_venues;  // bit v: venue v used cancel-replace (kHeaderSession)
  std::uint32_t config_crc32c;   // of the config text
  std::uint8_t reserved[120];
  std::uint32_t crc32c;  // over the preceding 252 bytes
};
static_assert(sizeof(JournalFileHeader) == 256 && std::is_trivially_copyable_v<JournalFileHeader>);

static_assert(offsetof(JournalFileHeader, session_epoch) == 112 &&
              offsetof(JournalFileHeader, crc32c) == 252);

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

  // Copies `msg` (hdr.len bytes) with hdr.seq assigned and no engine time (market-data journals,
  // latency samples). Returns the seq.
  Result<std::uint64_t, JournalError> record(const EventHeader& msg,
                                             std::uint8_t extra_flags = 0) noexcept {
    return put(msg, extra_flags, 0);
  }
  // A consumed event and the engine clock it was processed at: a kEngineTime delta, preceded by an
  // EngineTimeMsg when the delta does not fit in int32 ns or a full ring broke the chain.
  Result<std::uint64_t, JournalError> record_at(const EventHeader& msg,
                                                Timestamp engine_ts) noexcept {
    std::int64_t d = engine_ts.ns - last_engine_ns_;
    if (FASTMM_UNLIKELY(!clock_synced_ || d > INT32_MAX || d < INT32_MIN)) {
      if (auto r = record_clock(EngineTimeMsg::Kind::Sync, engine_ts); !r) return r;
      d = 0;
    }
    auto r = put(msg, EventHeader::kEngineTime, static_cast<std::uint32_t>(d));
    if (FASTMM_LIKELY(r.has_value())) last_engine_ns_ = engine_ts.ns;
    return r;
  }
  // The absolute engine clock (start, finish, delta overflow).
  Result<std::uint64_t, JournalError> record_clock(EngineTimeMsg::Kind kind,
                                                   Timestamp engine_ts) noexcept {
    EngineTimeMsg m{};
    init_header(m, EventType::EngineTime);
    m.hdr.flags = EventHeader::kSynthetic;
    m.engine_ts = engine_ts;
    m.kind = kind;
    auto r = put(m.hdr, 0, 0);
    if (FASTMM_LIKELY(r.has_value())) {
      last_engine_ns_ = engine_ts.ns;
      clock_synced_ = true;
    }
    return r;
  }
  // Outbound copy; `dropped` marks a message the transport did not accept.
  Result<std::uint64_t, JournalError> record_outbound(const EventHeader& msg,
                                                      bool dropped = false) noexcept {
    std::uint8_t flags = EventHeader::kOutbound;
    if (dropped) flags = static_cast<std::uint8_t>(flags | EventHeader::kDropped);
    return put(msg, flags, 0);
  }

 private:
  Result<std::uint64_t, JournalError> put(const EventHeader& msg,
                                          std::uint8_t set_flags,
                                          std::uint32_t reserved0) noexcept {
    const std::uint64_t seq = next_seq_++;
    if (ring_ == nullptr) return seq;
    std::byte* p = ring_->try_reserve(msg.len);
    if (FASTMM_UNLIKELY(p == nullptr)) {
      ++overflows_;
      clock_synced_ = false;
      return fail(JournalError::RingFull);
    }
    std::memcpy(p, &msg, msg.len);
    auto* h = reinterpret_cast<EventHeader*>(p);
    h->seq = seq;
    // The journal-only bits describe this record, never the history of a copied message.
    h->flags = static_cast<std::uint8_t>(
        (h->flags & ~(EventHeader::kEngineTime | EventHeader::kDropped)) | set_flags);
    h->reserved0 = reserved0;
    ring_->commit();
    return seq;
  }

  MsgRing* ring_;
  std::uint64_t next_seq_ = 1;
  std::uint64_t overflows_ = 0;
  std::int64_t last_engine_ns_ = 0;
  bool clock_synced_ = false;
};

struct JournalSessionInfo {
  std::uint64_t session_id = 0;
  Timestamp start_ts{};
  TscCalibration tsc{};
  std::uint64_t config_hash = 0;
  std::uint64_t rng_seed = 0;
  std::string_view strategy;
  const InstrumentTable* instruments = nullptr;
  // Engine session settings a replay restores (engine and backtest sessions; left unset for
  // market-data journals).
  bool has_session = false;
  std::uint16_t session_epoch = 0;
  bool quoting_enabled = true;
  std::uint64_t replace_venues = 0;  // bit v: venue v used cancel-replace
  std::string_view config_toml;      // effective configuration (Config::effective_toml())
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
  [[nodiscard]] std::uint32_t version() const noexcept { return header_->version; }
  // The v2 session settings (session_epoch, quoting_enabled, replace_venues) are present.
  [[nodiscard]] bool has_session() const noexcept {
    return header_->version >= 2 && (header_->header_flags & kHeaderSession) != 0;
  }
  // Effective configuration TOML embedded by the recording (empty: none, e.g. a v1 file).
  [[nodiscard]] std::string_view config_text() const noexcept { return config_; }
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
  std::string_view config_;
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
