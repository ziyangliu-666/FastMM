#include "fastmm/net/tls_stream.hpp"

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>

#include <arpa/inet.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace fastmm::net {

namespace {

[[noreturn]] void throw_openssl(const char* what) {
  char buf[256];
  ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
  ERR_clear_error();
  throw std::runtime_error(std::string(what) + ": " + buf);
}

bool is_ip_literal(std::string_view host) noexcept {
  char z[64];
  if (host.empty() || host.size() >= sizeof(z)) return false;
  std::memcpy(z, host.data(), host.size());
  z[host.size()] = '\0';
  unsigned char tmp[16];
  return ::inet_pton(AF_INET, z, tmp) == 1 || ::inet_pton(AF_INET6, z, tmp) == 1;
}

}  // namespace

// --------------------------------------------------------------------------- TlsContext

TlsContext::TlsContext(Mode mode) : mode_(mode) {
  ctx_ = SSL_CTX_new(mode == Mode::Client ? TLS_client_method() : TLS_server_method());
  if (ctx_ == nullptr) throw_openssl("SSL_CTX_new");
  SSL_CTX_set_min_proto_version(ctx_, TLS1_2_VERSION);
  SSL_CTX_set_options(ctx_, SSL_OP_NO_COMPRESSION);
  // Partial writes let TlsStream::write report exact byte counts under back-pressure.
  SSL_CTX_set_mode(ctx_, SSL_MODE_ENABLE_PARTIAL_WRITE | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
  if (mode == Mode::Client) {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_PEER, nullptr);
    if (SSL_CTX_set_default_verify_paths(ctx_) != 1)
      throw_openssl("SSL_CTX_set_default_verify_paths");
  } else {
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
  }
}

TlsContext::TlsContext() : TlsContext(Mode::Client) {}

TlsContext TlsContext::server(const std::string& cert_pem_path, const std::string& key_pem_path) {
  TlsContext ctx(Mode::Server);
  if (SSL_CTX_use_certificate_chain_file(ctx.ctx_, cert_pem_path.c_str()) != 1) {
    throw_openssl("SSL_CTX_use_certificate_chain_file");
  }
  if (SSL_CTX_use_PrivateKey_file(ctx.ctx_, key_pem_path.c_str(), SSL_FILETYPE_PEM) != 1) {
    throw_openssl("SSL_CTX_use_PrivateKey_file");
  }
  if (SSL_CTX_check_private_key(ctx.ctx_) != 1) throw_openssl("SSL_CTX_check_private_key");
  return ctx;
}

TlsContext::~TlsContext() {
  if (ctx_ != nullptr) SSL_CTX_free(ctx_);
}

void TlsContext::set_ca_file(const std::string& path) {
  if (SSL_CTX_load_verify_file(ctx_, path.c_str()) != 1) throw_openssl("SSL_CTX_load_verify_file");
}

// ---------------------------------------------------------------------------- TlsEngine

namespace detail {

TlsEngine::TlsEngine(TlsContext& ctx, std::string_view host) {
  ssl_ = SSL_new(ctx.native());
  if (ssl_ == nullptr) throw_openssl("SSL_new");
  BIO* internal = nullptr;
  if (BIO_new_bio_pair(&internal, kBioBufferSize, &net_bio_, kBioBufferSize) != 1) {
    SSL_free(ssl_);
    throw_openssl("BIO_new_bio_pair");
  }
  SSL_set_bio(ssl_, internal, internal);  // SSL owns `internal`; we own net_bio_

  if (ctx.mode() == TlsContext::Mode::Client) {
    SSL_set_connect_state(ssl_);
    if (ctx.insecure()) {
      SSL_set_verify(ssl_, SSL_VERIFY_NONE, nullptr);
    } else if (!host.empty()) {
      // Hostname verification (RFC 6125) via the verify params; IP literals are matched
      // against iPAddress SANs instead.
      X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
      const std::string host_z(host);
      const int ok = is_ip_literal(host)
                         ? X509_VERIFY_PARAM_set1_ip_asc(param, host_z.c_str())
                         : X509_VERIFY_PARAM_set1_host(param, host_z.c_str(), host_z.size());
      if (ok != 1) {
        SSL_free(ssl_);
        BIO_free(net_bio_);
        throw_openssl("X509_VERIFY_PARAM_set1_host");
      }
    }
    // SNI: RFC 6066 forbids IP literals in the server_name extension.
    if (!host.empty() && !is_ip_literal(host)) {
      // SSL_set_tlsext_host_name is a macro with a C-style cast; call the ctrl directly.
      std::string host_z(host);
      SSL_ctrl(ssl_, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, host_z.data());
    }
  } else {
    SSL_set_accept_state(ssl_);
  }
}

TlsEngine::~TlsEngine() {
  if (ssl_ != nullptr) SSL_free(ssl_);
  if (net_bio_ != nullptr) BIO_free(net_bio_);
}

TlsOp TlsEngine::map_result(int ret) noexcept {
  if (ret > 0) return TlsOp::Ok;
  const int err = SSL_get_error(ssl_, ret);
  switch (err) {
    case SSL_ERROR_WANT_READ:
      return TlsOp::WantRead;
    case SSL_ERROR_WANT_WRITE:
      return TlsOp::WantWrite;
    case SSL_ERROR_ZERO_RETURN:
      return TlsOp::Closed;
    case SSL_ERROR_SYSCALL:
      // With a BIO pair this only means an unexpected EOF (peer vanished mid-record).
      capture_error("tls: unexpected eof");
      return TlsOp::Error;
    default:
      capture_error("tls");
      return TlsOp::Error;
  }
}

void TlsEngine::capture_error(const char* prefix) noexcept {
  const unsigned long code = ERR_peek_last_error();
  char buf[128] = {};
  if (code != 0) ERR_error_string_n(code, buf, sizeof(buf));
  const long verify = SSL_get_verify_result(ssl_);
  if (verify != X509_V_OK) {
    std::snprintf(error_.text,
                  sizeof(error_.text),
                  "%s: %s (verify: %s)",
                  prefix,
                  buf,
                  X509_verify_cert_error_string(verify));
  } else {
    std::snprintf(error_.text, sizeof(error_.text), "%s: %s", prefix, buf);
  }
  ERR_clear_error();
}

TlsOp TlsEngine::handshake() noexcept {
  if (SSL_is_init_finished(ssl_)) return TlsOp::Ok;
  return map_result(SSL_do_handshake(ssl_));
}

TlsOp TlsEngine::read(std::span<std::byte> buf, std::size_t& n) noexcept {
  n = 0;
  if (buf.empty()) return TlsOp::Ok;
  const int ret = SSL_read_ex(ssl_, buf.data(), buf.size(), &n);
  return map_result(ret);
}

TlsOp TlsEngine::write(std::span<const std::byte> buf, std::size_t& n) noexcept {
  n = 0;
  if (buf.empty()) return TlsOp::Ok;
  const int ret = SSL_write_ex(ssl_, buf.data(), buf.size(), &n);
  return map_result(ret);
}

TlsOp TlsEngine::shutdown() noexcept {
  // 0: close_notify sent, peer's not yet received; 1: bidirectional shutdown complete.
  const int ret = SSL_shutdown(ssl_);
  if (ret == 1) return TlsOp::Ok;
  if (ret == 0) return TlsOp::WantRead;
  return map_result(ret);
}

bool TlsEngine::handshake_done() const noexcept {
  return SSL_is_init_finished(ssl_) != 0;
}

std::size_t TlsEngine::plaintext_pending() const noexcept {
  return static_cast<std::size_t>(SSL_pending(ssl_));
}

std::span<const std::byte> TlsEngine::ciphertext_out() noexcept {
  char* ptr = nullptr;
  const int n = BIO_nread0(net_bio_, &ptr);
  if (n <= 0 || ptr == nullptr) return {};
  return {reinterpret_cast<const std::byte*>(ptr), static_cast<std::size_t>(n)};
}

void TlsEngine::ciphertext_out_consumed(std::size_t n) noexcept {
  if (n == 0) return;
  char* ptr = nullptr;
  BIO_nread(net_bio_, &ptr, static_cast<int>(n));
}

std::span<std::byte> TlsEngine::ciphertext_in_space() noexcept {
  char* ptr = nullptr;
  const int n = BIO_nwrite0(net_bio_, &ptr);
  if (n <= 0 || ptr == nullptr) return {};
  return {reinterpret_cast<std::byte*>(ptr), static_cast<std::size_t>(n)};
}

void TlsEngine::ciphertext_in_commit(std::size_t n) noexcept {
  if (n == 0) return;
  char* ptr = nullptr;
  BIO_nwrite(net_bio_, &ptr, static_cast<int>(n));
}

std::string_view TlsEngine::protocol_version() const noexcept {
  return SSL_get_version(ssl_);
}

std::string_view TlsEngine::cipher_name() const noexcept {
  const char* name = SSL_get_cipher_name(ssl_);
  return name != nullptr ? name : "";
}

}  // namespace detail
}  // namespace fastmm::net
