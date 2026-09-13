#pragma once
// TLS over any ByteStream using an OpenSSL BIO pair (fixed-size in-memory BIOs):
//
//   app <-SSL_read/SSL_write-> [SSL] <-internal BIO | network BIO-> TlsStream <-> Transport
//
// OpenSSL never touches a socket: TlsStream moves ciphertext between the network BIO and
// the transport with zero-copy BIO_nread0/BIO_nwrite0 pointers. This makes TLS testable in
// one process (MemoryTransport) and keeps the same shape for a future kernel-bypass
// transport. The BIO pair buffers are allocated once at construction (no growth), so a slow
// socket back-pressures SSL_write (want_write) instead of growing memory.
//
// OpenSSL headers are not included here; TlsContext/TlsEngine wrap opaque handles.
#include "fastmm/net/byte_stream.hpp"
#include "fastmm/net/io_result.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

struct ssl_ctx_st;  // SSL_CTX
struct ssl_st;      // SSL
struct bio_st;      // BIO

namespace fastmm::net {

class TlsContext {
 public:
  enum class Mode : std::uint8_t { Client, Server };

  // Client context: TLS 1.2+, peer verification against the system CA store.
  TlsContext();  // throws std::runtime_error
  // Server context loading a PEM certificate chain + private key (sim server / tests).
  static TlsContext server(const std::string& cert_pem_path, const std::string& key_pem_path);
  ~TlsContext();
  TlsContext(TlsContext&& o) noexcept
      : ctx_(std::exchange(o.ctx_, nullptr)), mode_(o.mode_), insecure_(o.insecure_) {}
  TlsContext& operator=(TlsContext&&) = delete;
  TlsContext(const TlsContext&) = delete;
  TlsContext& operator=(const TlsContext&) = delete;

  // Trust only this PEM bundle (e.g. the sim server's self-signed certificate).
  void set_ca_file(const std::string& path);  // throws std::runtime_error
  // Disable certificate + hostname verification (sim/testing only).
  void set_insecure(bool insecure) noexcept { insecure_ = insecure; }
  bool insecure() const noexcept { return insecure_; }
  Mode mode() const noexcept { return mode_; }
  ssl_ctx_st* native() const noexcept { return ctx_; }

 private:
  explicit TlsContext(Mode mode);
  ssl_ctx_st* ctx_ = nullptr;
  Mode mode_ = Mode::Client;
  bool insecure_ = false;
};

namespace detail {

enum class TlsOp : std::uint8_t { Ok, WantRead, WantWrite, Closed, Error };

// Non-template OpenSSL state machine (SSL + BIO pair); lives in tls_stream.cpp.
class TlsEngine {
 public:
  static constexpr std::size_t kBioBufferSize = 64 * 1024;

  // Client mode: SNI + hostname verification for `host` (IP literals verified as IPs).
  TlsEngine(TlsContext& ctx, std::string_view host);  // throws std::runtime_error
  ~TlsEngine();
  TlsEngine(TlsEngine&& o) noexcept
      : ssl_(std::exchange(o.ssl_, nullptr)), net_bio_(std::exchange(o.net_bio_, nullptr)) {
    error_ = o.error_;
  }
  TlsEngine& operator=(TlsEngine&&) = delete;
  TlsEngine(const TlsEngine&) = delete;
  TlsEngine& operator=(const TlsEngine&) = delete;

  TlsOp handshake() noexcept;
  TlsOp read(std::span<std::byte> buf, std::size_t& n) noexcept;
  TlsOp write(std::span<const std::byte> buf, std::size_t& n) noexcept;
  TlsOp shutdown() noexcept;
  bool handshake_done() const noexcept;
  std::size_t plaintext_pending() const noexcept;  // SSL_pending

  // Network side of the BIO pair (zero-copy).
  std::span<const std::byte> ciphertext_out() noexcept;  // BIO_nread0
  void ciphertext_out_consumed(std::size_t n) noexcept;  // BIO_nread
  std::span<std::byte> ciphertext_in_space() noexcept;   // BIO_nwrite0
  void ciphertext_in_commit(std::size_t n) noexcept;     // BIO_nwrite

  std::string_view last_error() const noexcept { return error_.text; }
  std::string_view protocol_version() const noexcept;
  std::string_view cipher_name() const noexcept;

 private:
  struct ErrorText {
    char text[192] = {};
  };
  TlsOp map_result(int ret) noexcept;
  void capture_error(const char* prefix) noexcept;

  ssl_st* ssl_ = nullptr;
  bio_st* net_bio_ = nullptr;
  ErrorText error_;
};

}  // namespace detail

template <ByteStream Transport>
class TlsStream {
 public:
  // `host` is the SNI / verification name (ignored for server contexts).
  TlsStream(TlsContext& ctx, Transport&& transport, std::string_view host = {})
      : transport_(std::move(transport)), engine_(ctx, host) {}
  TlsStream(TlsStream&&) noexcept = default;
  TlsStream& operator=(TlsStream&&) = delete;

  // Drives transport handshake (TCP connect) then the TLS handshake. Call again on
  // readable/writable events while it reports want_read/want_write.
  IoResult handshake() noexcept {
    if (!transport_ready_) {
      const IoResult t = transport_.handshake();
      if (!t.ok() || t.would_block()) return t;
      transport_ready_ = true;
    }
    for (;;) {
      const detail::TlsOp op = engine_.handshake();
      const IoResult fl = flush();
      if (fl.failed()) return fl;
      switch (op) {
        case detail::TlsOp::Ok:
          return fl.want_write ? IoResult::wants_write() : IoResult::done(0);
        case detail::TlsOp::WantRead: {
          const IoResult in = pump_in();
          if (in.failed()) return in;
          if (in.bytes == 0)
            return fl.want_write ? IoResult::wants_write() : IoResult::wants_read();
          break;  // fed more ciphertext; retry the handshake
        }
        case detail::TlsOp::WantWrite:
          return IoResult::wants_write();  // BIO pair full until the transport drains
        case detail::TlsOp::Closed:
          return IoResult::eof();
        case detail::TlsOp::Error:
          return IoResult::failure(NetError::Tls);
      }
    }
  }

  // Loop until want_read (edge-triggered contract): each call returns at most one
  // SSL_read worth of plaintext.
  IoResult read(std::span<std::byte> buf) noexcept {
    for (;;) {
      std::size_t n = 0;
      const detail::TlsOp op = engine_.read(buf, n);
      switch (op) {
        case detail::TlsOp::Ok:
          flush();  // TLS 1.3 may queue key updates / ticket acks
          return IoResult::done(n);
        case detail::TlsOp::WantRead: {
          const IoResult in = pump_in();
          if (in.failed()) return in;
          if (in.bytes == 0) return IoResult::wants_read();
          break;
        }
        case detail::TlsOp::WantWrite: {
          const IoResult fl = flush();
          if (fl.failed()) return fl;
          if (fl.want_write) return IoResult::wants_write();
          break;
        }
        case detail::TlsOp::Closed:
          return IoResult::eof();
        case detail::TlsOp::Error:
          return IoResult::failure(NetError::Tls);
      }
    }
  }

  // Returns bytes accepted; want_write with bytes < buf.size() means retry the remainder
  // after the fd becomes writable (SSL keeps any partially written record internally).
  IoResult write(std::span<const std::byte> buf) noexcept {
    std::size_t total = 0;
    while (total < buf.size()) {
      std::size_t n = 0;
      const detail::TlsOp op = engine_.write(buf.subspan(total), n);
      total += n;
      IoResult fl = flush();
      if (fl.failed()) {
        fl.bytes = total;
        return fl;
      }
      switch (op) {
        case detail::TlsOp::Ok:
          break;
        case detail::TlsOp::WantWrite:
          if (fl.want_write) return IoResult::wants_write(total);
          break;  // the flush freed BIO space; retry
        case detail::TlsOp::WantRead:
          return IoResult::wants_read(total);
        case detail::TlsOp::Closed: {
          IoResult r = IoResult::eof();
          r.bytes = total;
          return r;
        }
        case detail::TlsOp::Error: {
          IoResult r = IoResult::failure(NetError::Tls);
          r.bytes = total;
          return r;
        }
      }
    }
    const IoResult fl = flush();
    if (fl.failed()) return fl;
    return fl.want_write ? IoResult::wants_write(total) : IoResult::done(total);
  }

  // Pushes buffered ciphertext to the transport (call on writable events).
  IoResult flush() noexcept {
    std::size_t total = 0;
    for (;;) {
      const std::span<const std::byte> out = engine_.ciphertext_out();
      if (out.empty()) return IoResult::done(total);
      const IoResult w = transport_.write(out);
      engine_.ciphertext_out_consumed(w.bytes);
      total += w.bytes;
      if (w.failed()) return w;
      if (w.want_write) return IoResult::wants_write(total);
    }
  }
  bool has_pending_output() noexcept { return !engine_.ciphertext_out().empty(); }

  // Sends close_notify; ok once the peer's close_notify was seen (or want_* meanwhile).
  IoResult shutdown() noexcept {
    const detail::TlsOp op = engine_.shutdown();
    const IoResult fl = flush();
    if (fl.failed()) return fl;
    if (op == detail::TlsOp::Ok) return fl.want_write ? IoResult::wants_write() : IoResult::done(0);
    if (op == detail::TlsOp::WantRead) {
      const IoResult in = pump_in();
      if (in.failed()) return in;
      return IoResult::wants_read();
    }
    if (op == detail::TlsOp::WantWrite) return IoResult::wants_write();
    return IoResult::failure(NetError::Tls);
  }

  int fd() const noexcept { return transport_.fd(); }
  void close() noexcept { transport_.close(); }
  bool handshake_done() const noexcept { return engine_.handshake_done(); }
  bool transport_ready() const noexcept { return transport_ready_; }

  Transport& transport() noexcept { return transport_; }
  const Transport& transport() const noexcept { return transport_; }
  detail::TlsEngine& engine() noexcept { return engine_; }
  std::string_view last_error() const noexcept { return engine_.last_error(); }

 private:
  // Moves ciphertext from the transport into the BIO pair. bytes == 0 with ok() means the
  // transport had nothing (want_read) or the BIO is full.
  IoResult pump_in() noexcept {
    std::size_t total = 0;
    for (;;) {
      const std::span<std::byte> space = engine_.ciphertext_in_space();
      if (space.empty()) return IoResult::done(total);
      const IoResult r = transport_.read(space);
      if (r.bytes > 0) {
        engine_.ciphertext_in_commit(r.bytes);
        total += r.bytes;
      }
      if (r.closed) {
        // EOF: let SSL consume what we already fed; only report EOF once it is drained.
        if (total == 0) return r;
        return IoResult::done(total);
      }
      if (r.failed()) return r;
      if (r.want_read) return IoResult::done(total);
    }
  }

  Transport transport_;
  detail::TlsEngine engine_;
  bool transport_ready_ = false;
};

static_assert(ByteStream<TlsStream<PlainStream>>);

}  // namespace fastmm::net
