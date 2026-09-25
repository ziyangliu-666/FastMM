#pragma once
// fastmm-gateway: the venue connections in a process of their own, and the strategy process that
// attaches to it (fastmm-live --gateway <socket>).
//
//   gateway                                   strategy (fastmm-live --gateway)
//   fm-net-<i>: connector, reactor            fm-engine: Engine<..., LiveTransport, RingFeed>
//     md sink    ── ShmRing <attach>-<v>.md ──►  RingFeed
//     order sink ── ShmRing <attach>-<v>.ord ─►  RingFeed
//     venue      ◄─ ShmRing <attach>-<v>.out ──  LiveTransport
//   main: AF_UNIX SOCK_SEQPACKET listener  ◄──── the attachment: one connection for its lifetime
//
// Attach: the strategy connects and sends an AttachRequest. The gateway creates three ShmRings per
// venue under /dev/shm, switches the venue's sinks to them on the venue's network thread, asks
// every book for a fresh snapshot, starts a reconciliation (executions, then open orders) and
// answers with an AttachReply: its instrument table (the reference data it loaded) and, per venue,
// the id, the name, whether it trades with cancel-replace and the three ring paths.
//
// Detach is the connection closing, which the kernel does when the strategy process dies, kill -9
// included: no heartbeat, no timeout. The gateway points the sinks back at local rings it discards
// (and counts), cancels every open order on every venue (Venue::cancel_all), removes the rings and
// waits for the next strategy. The venue connections stay up throughout. One strategy at a time.
//
// Wake-ups cross the process boundary the way they cross threads in fastmm-live. The attach reply
// carries descriptors (SCM_RIGHTS): a memfd page (gw::WakePage) and each venue's reactor eventfd.
//   gateway -> engine: the engine's feed Waker lives in the page (a shared futex). A network thread
//     notifies it after its sinks pushed, which costs a futex wake-up only while the engine sleeps.
//     Only when the strategy said it blocks (kStrategyBlocks).
//   engine -> gateway: an adaptive network thread sets its flag in the page before it blocks in
//     the reactor and rechecks the outbound ring; the engine takes the flag after it pushed and
//     writes the eventfd only if it was set. Only when the gateway said it blocks (kGatewayBlocks).
// With spin_mode = "busy" on both sides neither side touches the page.
//
// Wire format: little-endian structs of fixed size, one per datagram, each starting with a Header.
// A reply carries the request's version; a version or size the gateway does not know is refused
// with an error reply.
#include "fastmm/config/config.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/shm_ring.hpp"
#include "fastmm/core/strong_id.hpp"
#include "fastmm/core/thread_utils.hpp"

#include <sys/types.h>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace fastmm::live {

namespace gw {

static_assert(std::endian::native == std::endian::little);

inline constexpr std::uint32_t kMagic = 0x57474d46;  // "FMGW"
inline constexpr std::uint16_t kVersion = 2;

enum class MsgType : std::uint16_t {
  AttachRequest = 1,
  AttachReply = 2,
};

struct Header {
  std::uint32_t magic;
  std::uint16_t version;
  MsgType type;
  std::uint32_t bytes;  // the whole datagram
  std::uint32_t reserved;
};
static_assert(sizeof(Header) == 16);

// A venue trade id the strategy's store already holds (its execution replay skips it).
struct ExecId {
  char id[64];
};

// The strategy restores a position from its store: the venues' execution replay starts at
// exec_since_ms and skips the `known_count` ExecIds that follow the request.
inline constexpr std::uint32_t kResumeExecutions = 1U << 0;
// The strategy's engine blocks when idle (spin_mode = "adaptive"): the gateway wakes it.
inline constexpr std::uint32_t kStrategyBlocks = 1U << 1;
inline constexpr std::uint32_t kMaxKnownExecIds = 1024;

struct AttachRequest {
  Header hdr;
  char engine[64];  // [engine] name of the strategy, for the gateway's log
  std::uint32_t pid;
  std::uint32_t flags;
  std::int64_t exec_since_ms;  // venue time
  std::uint32_t known_count;
  std::uint32_t reserved;
};
static_assert(sizeof(AttachRequest) == 104);

inline constexpr std::uint8_t kVenueReplace = 1U << 0;     // trades with cancel-replace
inline constexpr std::uint8_t kVenueExecutions = 1U << 1;  // can replay executions

struct VenueInfo {
  std::uint8_t id;
  std::uint8_t flags;
  std::uint8_t reserved[6];
  char name[32];
  char md_path[128];
  char order_path[128];
  char outbound_path[128];
};
static_assert(sizeof(VenueInfo) == 424);

// The gateway's network threads block when idle (spin_mode = "adaptive"): the engine wakes them.
inline constexpr std::uint32_t kGatewayBlocks = 1U << 0;

// Followed by venue_count VenueInfo, then instrument_count Instrument (instrument_bytes each).
// An attached reply carries 1 + venue_count descriptors: the WakePage memfd, then the reactor
// eventfd of each venue in id order.
struct AttachReply {
  Header hdr;
  std::int32_t status;  // 0: attached; otherwise `error` says why
  std::uint32_t attach_id;
  std::uint32_t venue_count;
  std::uint32_t instrument_count;
  std::uint32_t instrument_bytes;  // sizeof(Instrument) of the gateway's build
  std::uint32_t gateway_pid;
  std::uint32_t flags;
  char error[228];
};
static_assert(sizeof(AttachReply) == 272);

// The sleeping flags of one attachment, in a memfd page both processes map, each flag on its own
// cache line. The page goes away with the attachment.
struct WakePage {
  struct alignas(64) Line {
    SleepFlag flag;
  };
  Line engine;  // the strategy's feed Waker (a shared futex)
  Line net[8];  // venue i's network thread is blocked in its reactor
};
inline constexpr std::size_t kWakePageBytes = 4096;
static_assert(sizeof(WakePage) <= kWakePageBytes);

// A zeroed WakePage in a new memfd (close-on-exec): the descriptor, or -1 with errno set.
[[nodiscard]] int create_wake_page() noexcept;
// Maps the page behind `fd` (MAP_SHARED); nullptr on failure. unmap_wake_page() undoes it.
[[nodiscard]] WakePage* map_wake_page(int fd) noexcept;
void unmap_wake_page(WakePage* page) noexcept;

// The producer's side of a blocked reactor: after publishing, take the consumer's flag and, if it
// was set, write 1 to its eventfd.
void wake_if_blocked(SleepFlag& flag, int eventfd) noexcept;

// One datagram with descriptors attached (SCM_RIGHTS). The bytes sent, or -1 with errno set.
ssize_t send_with_fds(int sock, const void* data, std::size_t len, std::span<const int> fds);
// One datagram and up to `max_fds` descriptors (close-on-exec); `*nfds` receives their count.
// The kernel closes descriptors beyond max_fds (MSG_CTRUNC).
ssize_t recv_with_fds(
    int sock, void* data, std::size_t len, int* fds, std::size_t max_fds, std::size_t* nfds);

inline constexpr std::size_t kMaxDatagram =
    sizeof(AttachReply) + 8 * sizeof(VenueInfo) + kMaxInstruments * sizeof(Instrument);

}  // namespace gw

// ---- the gateway ------------------------------------------------------------------------------

struct GatewayOptions {
  std::string socket_path;       // empty: <journal_dir>/<engine name>.gw
  std::int64_t duration_ns = 0;  // 0: until SIGINT/SIGTERM
  bool dry_run = false;
  std::string program = "fastmm-gateway";
};

// <journal_dir>/<engine name>.gw
[[nodiscard]] std::string default_gateway_path(const Config& cfg);

// Runs the gateway until SIGINT/SIGTERM or the duration; returns the exit code (the kExit* codes
// of live/session.hpp). `cfg` must have its venue secrets resolved (resolve_venue_env).
int run_gateway(const Config& cfg, const GatewayOptions& opts);

// ---- the strategy's side ----------------------------------------------------------------------

struct GatewayVenue {
  VenueId id;
  std::string name;
  bool replace = false;
  bool executions = false;
  std::unique_ptr<ShmRing> md;
  std::unique_ptr<ShmRing> order;
  std::unique_ptr<ShmRing> outbound;
};

struct GatewayAttachRequest {
  std::string engine;
  bool blocks = false;  // the engine runs with spin_mode = "adaptive"
  bool resume_executions = false;
  std::int64_t exec_since_ms = 0;
  std::vector<std::string> known_exec_ids;
};

// One attachment. The connection stays open for as long as this object lives; closing it (the
// destructor, or the process ending) is the detach.
class GatewayClient {
 public:
  // Connects to `path`, attaches and maps the rings. nullptr with the reason in `error`.
  [[nodiscard]] static std::unique_ptr<GatewayClient> attach(const std::string& path,
                                                             const GatewayAttachRequest& req,
                                                             std::string* error);
  GatewayClient(const GatewayClient&) = delete;
  GatewayClient& operator=(const GatewayClient&) = delete;
  ~GatewayClient();

  [[nodiscard]] const InstrumentTable& instruments() const noexcept { return instruments_; }
  [[nodiscard]] std::vector<GatewayVenue>& venues() noexcept { return venues_; }
  [[nodiscard]] std::uint32_t attach_id() const noexcept { return attach_id_; }
  // False once the gateway closed the connection (it exited or dropped this attachment). Never
  // blocks.
  [[nodiscard]] bool connected() noexcept;
  // Closes the connection (the detach). The wake page and the eventfds stay until the destructor,
  // so the engine's feed Waker and the wake hook remain valid.
  void close() noexcept;

  // The engine's feed Waker lives here: Waker::share(engine_flag()) before the engine starts.
  [[nodiscard]] SleepFlag* engine_flag() noexcept { return &page_->engine.flag; }
  // The gateway's network threads block when idle: install wake_venue as LiveTransport's hook.
  [[nodiscard]] bool gateway_blocks() const noexcept { return gateway_blocks_; }
  // LiveTransport::WakeFn (ctx: this client): after the engine pushed into venue v's outbound
  // ring, writes the venue's eventfd if its network thread is blocked.
  static void wake_venue(void* ctx, VenueId v) noexcept;

 private:
  GatewayClient() = default;
  int fd_ = -1;
  std::uint32_t attach_id_ = 0;
  gw::WakePage* page_ = nullptr;
  int wake_fds_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  std::size_t wake_count_ = 0;
  bool gateway_blocks_ = false;
  InstrumentTable instruments_;
  std::vector<GatewayVenue> venues_;
};

}  // namespace fastmm::live
