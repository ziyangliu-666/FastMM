// fastmm_live._live: extension module of the fastmm-live wheel. It exchanges only Python objects
// with fastmm._core and binds no C++ type (ADR-0013, section 5).
#include "self_test_pem.hpp"

#include "fastmm/net/ca_locations.hpp"
#include "fastmm/net/io_result.hpp"
#include "fastmm/net/memory_transport.hpp"
#include "fastmm/net/tls_stream.hpp"
#include "fastmm/version.hpp"

#include <openssl/crypto.h>
#include <pybind11/pybind11.h>

#include <cstddef>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

namespace py = pybind11;
namespace net = fastmm::net;
using MemTls = net::TlsStream<net::MemoryTransport>;

bool finished(const net::IoResult& r) noexcept {
  return r.ok() && !r.would_block();
}

// Empty on success, otherwise the side and OpenSSL's error text.
std::string handshake(MemTls& client, MemTls& server) {
  for (int i = 0; i < 64; ++i) {
    const net::IoResult c = client.handshake();
    const net::IoResult s = server.handshake();
    if (c.failed()) return "client: " + std::string(client.last_error());
    if (s.failed()) return "server: " + std::string(server.last_error());
    if (finished(c) && finished(s)) return {};
  }
  return "handshake did not finish in 64 steps";
}

std::string round_trip(MemTls& from, MemTls& to, std::string_view payload) {
  const net::IoResult w =
      from.write({reinterpret_cast<const std::byte*>(payload.data()), payload.size()});
  if (w.failed() || w.bytes != payload.size())
    throw std::runtime_error("self-test: TLS write failed");
  std::string received;
  std::string buf(256, '\0');
  for (int i = 0; i < 16 && received.size() < payload.size(); ++i) {
    const net::IoResult r = to.read({reinterpret_cast<std::byte*>(buf.data()), buf.size()});
    if (r.failed()) throw std::runtime_error("self-test: TLS read failed");
    received.append(buf, 0, r.bytes);
  }
  return received;
}

struct SelfTestResult {
  std::string protocol;
  std::string cipher;
  net::CaLocations ca;
};

// Throws std::runtime_error (RuntimeError in Python) naming the step that failed.
SelfTestResult run_self_test() {
  SelfTestResult out;
  // 1. The CA lookup a live session uses: a client context loads what it selects.
  out.ca = net::find_ca_locations();
  { const net::TlsContext system_trust; }

  // 2. TLS 1.3 handshake over an in-memory link against the test certificate, then one record each
  //    way.
  net::TlsContext server_ctx = net::TlsContext::server_pem(fastmm::py_live::kSelfTestCertPem,
                                                           fastmm::py_live::kSelfTestKeyPem);
  net::TlsContext client_ctx;
  client_ctx.add_ca_pem(fastmm::py_live::kSelfTestCertPem);
  {
    net::MemoryLink link;
    MemTls client(client_ctx, link.end_a(), "localhost");
    MemTls server(server_ctx, link.end_b());
    const std::string err = handshake(client, server);
    if (!err.empty()) throw std::runtime_error("self-test: handshake failed: " + err);
    if (round_trip(client, server, "ping") != "ping" ||
        round_trip(server, client, "pong") != "pong") {
      throw std::runtime_error("self-test: TLS round trip returned other bytes");
    }
    out.protocol = std::string(client.engine().protocol_version());
    out.cipher = std::string(client.engine().cipher_name());
  }

  // 3. Certificate verification is on: a client that does not trust the test certificate fails.
  {
    net::TlsContext untrusting;
    net::MemoryLink link;
    MemTls client(untrusting, link.end_a(), "localhost");
    MemTls server(server_ctx, link.end_b());
    if (handshake(client, server).empty()) {
      throw std::runtime_error("self-test: a client without the test CA completed the handshake");
    }
  }
  return out;
}

py::dict ca_dict(const net::CaLocations& ca) {
  py::dict d;
  d["source"] = std::string(net::ca_source_name(ca.source));
  d["file"] = ca.file;
  d["dir"] = ca.dir;
  return d;
}

}  // namespace

PYBIND11_MODULE(_live, m) {
  m.doc() = "FastMM live runtime: network stack, venue connectors and OpenSSL";
  m.attr("__version__") = fastmm::kVersionString;

  m.def(
      "build_info",
      [] {
        py::dict d;
        d["version"] = fastmm::kVersionString;
        d["build"] = std::string(fastmm::build_info());
        d["openssl"] = std::string(OpenSSL_version(OPENSSL_VERSION));
        d["openssl_static"] = FASTMM_OPENSSL_STATIC != 0;
        d["pybind11"] = std::to_string(PYBIND11_VERSION_MAJOR) + "." +
                        std::to_string(PYBIND11_VERSION_MINOR) + "." +
                        std::to_string(PYBIND11_VERSION_PATCH);
        d["pybind11_internals"] = std::string(PYBIND11_INTERNALS_ID);
        d["python"] = std::string(PY_VERSION);
        return d;
      },
      "Version, compiler and flags, the OpenSSL linked in, and the pybind11 and CPython ABI it was "
      "built for.");

  m.def(
      "set_ca_fallback_file",
      [](const std::string& path) { net::set_ca_fallback_file(path); },
      py::arg("path"),
      "CA bundle used when SSL_CERT_FILE, SSL_CERT_DIR and the system bundles are absent.");
  m.def("ca_fallback_file", [] { return net::ca_fallback_file(); });
  m.def(
      "ca_locations",
      [] { return ca_dict(net::find_ca_locations()); },
      "Where TLS clients load CA certificates from: {'source', 'file', 'dir'}.");

  m.def(
      "self_test",
      [] {
        SelfTestResult r;
        {
          const py::gil_scoped_release release;
          r = run_self_test();
        }
        py::dict d;
        d["protocol"] = r.protocol;
        d["cipher"] = r.cipher;
        d["openssl"] = std::string(OpenSSL_version(OPENSSL_VERSION));
        d["ca"] = ca_dict(r.ca);
        return d;
      },
      "Loads the CA certificates a live session would use, runs a TLS handshake and a round trip "
      "in memory, and checks that an untrusted certificate is rejected. Raises RuntimeError on "
      "failure.");
}
