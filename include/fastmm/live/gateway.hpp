#pragma once
// fastmm-gateway: the venue connections in a process of their own, and the strategy processes
// that attach to it (fastmm-live --gateway <socket>), several at once.
//
//   gateway                                   strategy (fastmm-live --gateway), one per attachment
//   fm-net-<i>: connector, reactor, router    fm-engine: Engine<..., LiveTransport, RingFeed>
//     md, to every attachment ── ShmRing <attach>-<v>.md ──►  RingFeed
//     order events, by epoch ─── ShmRing <attach>-<v>.ord ─►  RingFeed
//     venue ◄─ account guards ◄─ ShmRing <attach>-<v>.out ──  LiveTransport
//   main: AF_UNIX SOCK_SEQPACKET listener  ◄──── each attachment: one connection for its lifetime
//
// Attach: the strategy connects and sends an AttachRequest naming the instruments it trades; the
// gateway refuses it when a live attachment owns one of them. Otherwise it gives it a session
// epoch from its own epoch file (the high 16 bits of every client order id, so epochs are unique
// across attachments and across gateway restarts), creates three ShmRings per venue under
// /dev/shm, adds the attachment to every venue's router on the venue's network thread, asks every
// book for a fresh snapshot, starts a reconciliation (executions, then open orders) and answers
// with an AttachReply: the epoch, the instrument table (the reference data it loaded) and, per
// venue, the id, the name, whether it trades with cancel-replace and the three ring paths.
//
// Routing, on each venue's network thread. The connector's sinks write into local rings whose
// drain hook runs on every commit and copies each event out:
//   * market data to every attachment. A full md ring drops for that attachment only (counted);
//     it then gets a Resyncing state of its own and the venue's books are snapshotted again.
//   * an order event to the attachment whose epoch its client order id carries. A fill of an
//     epoch no attachment holds (a dead session's, or an execution naming no order) goes to the
//     owner of the instrument, and so does an account-level Position record.
//   * a reconciliation (Begin, rows, End) to the attachments that asked for one (their attach,
//     their engine's Reconcile), or to all when the connector started it itself. Each gets its own
//     rows only, under a Begin whose sent watermark is its own last order the venue had taken. A
//     row of an epoch no attachment holds is a dead session's order, which the gateway cancels.
// Outbound, the network thread moves each attachment's orders into the venue's own ring after the
// account guards ([gateway]: the order rate per venue, the notional working at the venue, the
// instrument belongs to the sender); a refused order goes back to its sender as an OrderReject.
//
// Detach is the connection closing, which the kernel does when the strategy process dies, kill -9
// included: no heartbeat, no timeout. The gateway takes the attachment out of the routers, cancels
// the orders of its epoch one by one (the connectors' venue-wide cancel_all would take every other
// strategy's quotes too), asks the venue for its open orders so that one it missed is swept too,
// frees its instruments and removes its rings. The venue connections stay up throughout.
//
// Wake-ups cross the process boundary the way they cross threads in fastmm-live. The attach reply
// carries descriptors (SCM_RIGHTS): the attachment's memfd page, the gateway's memfd page and each
// venue's reactor eventfd.
//   gateway -> engine: the engine's feed Waker lives in its attachment's page (a shared futex). A
//     network thread notifies it after it routed events there, which costs a futex wake-up only
//     while the engine sleeps. Only when the strategy said it blocks (kStrategyBlocks).
//   engine -> gateway: an adaptive network thread sets its flag in the gateway's page before it
//     blocks in the reactor and rechecks every outbound ring; an engine takes the flag after it
//     pushed and writes the eventfd only if it was set. Only when the gateway said it blocks
//     (kGatewayBlocks).
// With spin_mode = "busy" on both sides neither side touches the pages.
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
#include <utility>
#include <vector>

namespace fastmm::live {

namespace gw {

static_assert(std::endian::native == std::endian::little);

inline constexpr std::uint32_t kMagic = 0x57474d46;  // "FMGW"
inline constexpr std::uint16_t kVersion = 3;

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

// An instrument the strategy trades: the gateway's venue name and the symbol.
struct InstrumentClaim {
  char venue[32];
  char symbol[32];
};
static_assert(sizeof(InstrumentClaim) == 64);

// The strategy restores a position from its store: the venues' execution replay starts at
// exec_since_ms and skips the `known_count` ExecIds that follow the request.
inline constexpr std::uint32_t kResumeExecutions = 1U << 0;
// The strategy's engine blocks when idle (spin_mode = "adaptive"): the gateway wakes it.
inline constexpr std::uint32_t kStrategyBlocks = 1U << 1;
inline constexpr std::uint32_t kMaxKnownExecIds = 1024;

// Followed by known_count ExecId, then claim_count InstrumentClaim.
struct AttachRequest {
  Header hdr;
  char engine[64];  // [engine] name of the strategy, for the gateway's log
  std::uint32_t pid;
  std::uint32_t flags;
  std::int64_t exec_since_ms;  // venue time
  std::uint32_t known_count;
  std::uint32_t claim_count;
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
// An attached reply carries 2 + venue_count descriptors: the attachment's WakePage memfd, the
// gateway's WakePage memfd, then the reactor eventfd of each venue in id order.
struct AttachReply {
  Header hdr;
  std::int32_t status;  // 0: attached; otherwise `error` says why
  std::uint32_t attach_id;
  std::uint32_t venue_count;
  std::uint32_t instrument_count;
  std::uint32_t instrument_bytes;  // sizeof(Instrument) of the gateway's build
  std::uint32_t gateway_pid;
  std::uint32_t flags;
  std::uint16_t session_epoch;  // the epoch of this attachment's client order ids
  std::uint16_t reserved;
  char error[224];
};
static_assert(sizeof(AttachReply) == 272);

// Sleeping flags in a memfd page both processes map, each flag on its own cache line. Each
// attachment has a page for its engine's flag, which goes away with it; the gateway has one page
// for its network threads' flags, which every attachment maps.
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
inline constexpr std::size_t kMaxRequest = sizeof(AttachRequest) +
                                           kMaxKnownExecIds * sizeof(ExecId) +
                                           kMaxInstruments * sizeof(InstrumentClaim);
// Strategies attached to one gateway at once.
inline constexpr std::size_t kMaxAttachments = 16;

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
  // The instruments this strategy trades, (venue name, symbol). The gateway routes their fills and
  // position records here and refuses the attach when another attachment owns one of them.
  std::vector<std::pair<std::string, std::string>> instruments;
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
  // The session epoch the gateway gave this attachment: the engine's client order ids carry it.
  [[nodiscard]] std::uint16_t session_epoch() const noexcept { return epoch_; }
  // False once the gateway closed the connection (it exited or dropped this attachment). Never
  // blocks.
  [[nodiscard]] bool connected() noexcept;
  // Closes the connection (the detach). The wake pages and the eventfds stay until the destructor,
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
  std::uint16_t epoch_ = 0;
  gw::WakePage* page_ = nullptr;      // this attachment's: its engine's flag
  gw::WakePage* net_page_ = nullptr;  // the gateway's: its network threads' flags
  int wake_fds_[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
  std::size_t wake_count_ = 0;
  bool gateway_blocks_ = false;
  InstrumentTable instruments_;
  std::vector<GatewayVenue> venues_;
};

}  // namespace fastmm::live
