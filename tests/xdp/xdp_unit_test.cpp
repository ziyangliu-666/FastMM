// AF_XDP backend pieces that run without privileges: the frame parser, the BPF assembler, the
// filter program's structure and its behaviour under the interpreter in bpf_interp.hpp, and the
// private kernel ABI layouts against the build machine's UAPI headers.
#include "../../src/net/xdp_uapi.hpp"
#include "bpf_interp.hpp"
#include "test_support.hpp"
#include "xdp_test_util.hpp"

#include "fastmm/net/bpf_asm.hpp"
#include "fastmm/net/udp_frame.hpp"
#include "fastmm/net/xdp_datagram_source.hpp"
#include "fastmm/net/xdp_program.hpp"

#include <linux/bpf.h>
#include <linux/capability.h>
#include <linux/if_link.h>

#include <cstring>
#include <string_view>

using namespace fastmm::net;
using fastmm::net::test::build_frame;
using fastmm::net::test::FrameSpec;
using fastmm::net::test::ip;

namespace {

namespace b = fastmm::net::bpf;

constexpr xdp::MapFds kFakeFds{
    test::BpfEnv::kSubsFd, test::BpfEnv::kXsksFd, test::BpfEnv::kFallbackFd};

std::string_view text(std::span<const std::byte> s) {
  return {reinterpret_cast<const char*>(s.data()), s.size()};
}

test::BpfEnv env_for(std::span<const std::byte> frame) {
  test::BpfEnv env;
  env.packet = frame;
  for (const auto& k : test::case_keys()) env.subs.insert(test::key_bits(k));
  return env;
}

}  // namespace

// ---- kernel ABI ---------------------------------------------------------------------------------

static_assert(detail::kBpfMapCreate == BPF_MAP_CREATE);
static_assert(detail::kBpfMapLookupElem == BPF_MAP_LOOKUP_ELEM);
static_assert(detail::kBpfMapUpdateElem == BPF_MAP_UPDATE_ELEM);
static_assert(detail::kBpfProgLoad == BPF_PROG_LOAD);
static_assert(detail::kBpfProgTestRun == BPF_PROG_TEST_RUN);
static_assert(detail::kBpfMapTypeHash == BPF_MAP_TYPE_HASH);
static_assert(detail::kBpfMapTypePercpuArray == BPF_MAP_TYPE_PERCPU_ARRAY);
static_assert(detail::kBpfMapTypeXskmap == BPF_MAP_TYPE_XSKMAP);
static_assert(detail::kBpfProgTypeXdp == BPF_PROG_TYPE_XDP);
static_assert(detail::kXdpFlagsSkbMode == XDP_FLAGS_SKB_MODE);
static_assert(detail::kXdpFlagsDrvMode == XDP_FLAGS_DRV_MODE);
static_assert(xdp::kHelperMapLookupElem == BPF_FUNC_map_lookup_elem);
static_assert(xdp::kHelperRedirectMap == BPF_FUNC_redirect_map);
static_assert(static_cast<std::size_t>(xdp::kMdData) == offsetof(xdp_md, data));
static_assert(static_cast<std::size_t>(xdp::kMdDataEnd) == offsetof(xdp_md, data_end));
static_assert(static_cast<std::size_t>(xdp::kMdRxQueueIndex) == offsetof(xdp_md, rx_queue_index));
static_assert(xdp::kPass == XDP_PASS && xdp::kRedirect == XDP_REDIRECT);
static_assert(b::kPseudoMapFd == BPF_PSEUDO_MAP_FD);
static_assert(sizeof(b::Insn) == sizeof(bpf_insn));
static_assert(detail::kXdpPacketHeadroom == XDP_PACKET_HEADROOM);
static_assert(offsetof(detail::BpfMapCreateAttr, map_name) == offsetof(bpf_attr, map_name));
static_assert(offsetof(detail::BpfMapCreateAttr, map_ifindex) == offsetof(bpf_attr, map_ifindex));
static_assert(offsetof(detail::BpfMapElemAttr, value) == offsetof(bpf_attr, value));
static_assert(offsetof(detail::BpfMapElemAttr, flags) == offsetof(bpf_attr, flags));
static_assert(offsetof(detail::BpfProgLoadAttr, log_buf) == offsetof(bpf_attr, log_buf));
static_assert(offsetof(detail::BpfProgLoadAttr, prog_name) == offsetof(bpf_attr, prog_name));
static_assert(offsetof(detail::BpfProgLoadAttr, expected_attach_type) ==
              offsetof(bpf_attr, expected_attach_type));
static_assert(offsetof(detail::BpfTestRunAttr, retval) == offsetof(bpf_attr, test.retval));
static_assert(offsetof(detail::BpfTestRunAttr, data_in) == offsetof(bpf_attr, test.data_in));
static_assert(offsetof(detail::BpfTestRunAttr, repeat) == offsetof(bpf_attr, test.repeat));
static_assert(offsetof(detail::BpfTestRunAttr, duration) == offsetof(bpf_attr, test.duration));
static_assert(offsetof(detail::BpfLinkCreateAttr, attach_type) ==
              offsetof(bpf_attr, link_create.attach_type));
static_assert(offsetof(detail::BpfLinkCreateAttr, flags) == offsetof(bpf_attr, link_create.flags));
static_assert(detail::kCapNetAdmin == CAP_NET_ADMIN && detail::kCapNetRaw == CAP_NET_RAW &&
              detail::kCapIpcLock == CAP_IPC_LOCK && detail::kCapSysAdmin == CAP_SYS_ADMIN);
static_assert(detail::kCapabilityVersion3 == _LINUX_CAPABILITY_VERSION_3);
#ifdef CAP_BPF
static_assert(detail::kCapBpf == CAP_BPF);
#endif

TEST_CASE("abi: enum values present only as enums match the kernel") {
  // BPF_LINK_CREATE and BPF_XDP are enumerators, not macros.
  CHECK(detail::kBpfLinkCreate == static_cast<int>(BPF_LINK_CREATE));
  CHECK(detail::kBpfXdp == static_cast<std::uint32_t>(BPF_XDP));
  CHECK(offsetof(detail::XdpStatistics, rx_fill_ring_empty_descs) ==
        offsetof(xdp_statistics, rx_fill_ring_empty_descs));
  CHECK(offsetof(detail::XdpRingOffset, flags) == offsetof(xdp_ring_offset, flags));
  CHECK(offsetof(detail::XdpUmemReg, flags) == offsetof(xdp_umem_reg, flags));
}

// ---- frame parser -------------------------------------------------------------------------------

TEST_CASE("frame: plain and 802.1Q UDP frames parse") {
  for (const bool vlan : {false, true}) {
    FrameSpec s;
    s.vlan = vlan;
    s.payload = "ITCH";
    const auto f = build_frame(s);
    const UdpFrame p = parse_udp_frame(f, true);
    REQUIRE(p.status == FrameStatus::Ok);
    CHECK(p.vlan == vlan);
    CHECK(p.src_ip == ip("10.203.0.1"));
    CHECK(p.dst_ip == ip("239.10.0.1"));
    CHECK(p.src_port == 40000);
    CHECK(p.dst_port == 31001);
    CHECK(text(p.payload) == "ITCH");
  }
}

TEST_CASE("frame: IPv4 options move the UDP header") {
  for (const int ihl : {6, 10, 15}) {
    FrameSpec s;
    s.ihl = static_cast<std::uint8_t>(ihl);
    s.payload = "opt";
    const auto f = build_frame(s);
    const UdpFrame p = parse_udp_frame(f, true);
    REQUIRE(p.status == FrameStatus::Ok);
    CHECK(p.dst_port == 31001);
    CHECK(text(p.payload) == "opt");
  }
}

TEST_CASE("frame: first-stage rejections") {
  const auto status = [](const FrameSpec& s) { return parse_udp_frame(build_frame(s)).status; };
  FrameSpec s;
  s.ethertype = 0x86DD;
  CHECK(status(s) == FrameStatus::NotIpv4);
  s = {};
  s.vlan = true;
  s.inner_ethertype = 0x8100;
  CHECK(status(s) == FrameStatus::NotIpv4);  // one tag only
  s = {};
  s.ethertype = 0x88A8;
  CHECK(status(s) == FrameStatus::NotIpv4);
  s = {};
  s.version = 6;
  CHECK(status(s) == FrameStatus::BadIpHeader);
  s = {};
  s.ihl = 4;
  CHECK(status(s) == FrameStatus::BadIpHeader);
  s = {};
  s.frag = 0x2000;
  CHECK(status(s) == FrameStatus::Fragment);
  s.frag = 0x0001;
  CHECK(status(s) == FrameStatus::Fragment);
  s.frag = 0x8000;  // reserved bit alone is not a fragment
  CHECK(status(s) == FrameStatus::Ok);
  s = {};
  s.proto = 6;
  CHECK(status(s) == FrameStatus::NotUdp);
}

TEST_CASE("frame: lengths") {
  const auto status = [](const FrameSpec& s) { return parse_udp_frame(build_frame(s)).status; };
  FrameSpec s;
  s.tot_len_delta = 1;
  CHECK(status(s) == FrameStatus::BadLength);
  s.tot_len_delta = -9;  // below IHL + 8
  s.payload = "";
  CHECK(status(s) == FrameStatus::BadLength);
  s = {};
  s.udp_len_delta = 1;
  CHECK(status(s) == FrameStatus::BadLength);
  s.udp_len_delta = -6;  // UDP length 7
  CHECK(status(s) == FrameStatus::BadLength);
  s = {};
  s.udp_len_delta = -2;  // shorter than the IPv4 payload: the kernel trims, so do we
  const auto f = build_frame(s);
  const UdpFrame p = parse_udp_frame(f);
  REQUIRE(p.status == FrameStatus::Ok);
  CHECK(text(p.payload) == "hel");
  s = {};
  s.payload = "";
  s.trailer = 18;  // minimum-size Ethernet frame padding
  const auto padded_frame = build_frame(s);
  const UdpFrame padded = parse_udp_frame(padded_frame, true);
  REQUIRE(padded.status == FrameStatus::Ok);
  CHECK(padded.payload.empty());
}

TEST_CASE("frame: every truncation of a valid frame is rejected") {
  for (const bool vlan : {false, true}) {
    FrameSpec s;
    s.vlan = vlan;
    s.ihl = 7;
    const auto f = build_frame(s);
    for (std::size_t n = 0; n < f.size(); ++n) {
      const UdpFrame p = parse_udp_frame(std::span<const std::byte>(f).first(n));
      CHECK_MESSAGE(p.status != FrameStatus::Ok, "prefix " << n);
      if (n < (vlan ? 18U : 14U) + 28 + 8)
        CHECK_MESSAGE(p.status == FrameStatus::Truncated, "prefix " << n);
    }
    CHECK(parse_udp_frame(f).status == FrameStatus::Ok);
  }
}

TEST_CASE("frame: checksums only with verification") {
  FrameSpec s;
  s.bad_udp_checksum = true;
  CHECK(parse_udp_frame(build_frame(s), false).status == FrameStatus::Ok);
  CHECK(parse_udp_frame(build_frame(s), true).status == FrameStatus::BadChecksum);
  s = {};
  s.bad_ip_checksum = true;
  CHECK(parse_udp_frame(build_frame(s), true).status == FrameStatus::BadChecksum);
  s = {};
  s.udp_checksum = false;  // 0: not computed
  CHECK(parse_udp_frame(build_frame(s), true).status == FrameStatus::Ok);
  s = {};
  s.payload = "odd";
  CHECK(parse_udp_frame(build_frame(s), true).status == FrameStatus::Ok);
  s.ihl = 9;
  CHECK(parse_udp_frame(build_frame(s), true).status == FrameStatus::Ok);
}

TEST_CASE("frame: UDP checksum matches a known datagram") {
  // 192.0.2.1:1234 -> 239.1.1.1:5678, payload "abc"; reference value from an independent
  // RFC 1071 implementation.
  const std::byte udp[] = {std::byte{0x04},
                           std::byte{0xD2},
                           std::byte{0x16},
                           std::byte{0x2E},
                           std::byte{0x00},
                           std::byte{0x0B},
                           std::byte{0x00},
                           std::byte{0x00},
                           std::byte{'a'},
                           std::byte{'b'},
                           std::byte{'c'}};
  CHECK(udp_checksum(ip("192.0.2.1"), ip("239.1.1.1"), udp) == 0x6E71);
}

// ---- assembler ----------------------------------------------------------------------------------

namespace {
std::array<std::uint8_t, 8> bytes_of(const b::Insn& i) {
  std::array<std::uint8_t, 8> out{};
  std::memcpy(out.data(), &i, 8);
  return out;
}
using Bytes = std::array<std::uint8_t, 8>;
}  // namespace

TEST_CASE("bpf_asm: instruction encodings") {
  // Reference bytes as llvm-objdump prints them for the same instructions.
  CHECK(bytes_of(b::mov64_imm(b::R0, 2)) == Bytes{0xb7, 0x00, 0, 0, 0x02, 0, 0, 0});
  CHECK(bytes_of(b::mov64_reg(b::R6, b::R1)) == Bytes{0xbf, 0x16, 0, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::ldx(b::kW, b::R2, b::R6, 0)) == Bytes{0x61, 0x62, 0, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::ldx(b::kH, b::R5, b::R2, 12)) == Bytes{0x69, 0x25, 0x0c, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::ldx(b::kB, b::R5, b::R2, 14)) == Bytes{0x71, 0x25, 0x0e, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::ldx(b::kDw, b::R1, b::R0, 0)) == Bytes{0x79, 0x01, 0, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::stx(b::kW, b::R10, -8, b::R4)) == Bytes{0x63, 0x4a, 0xf8, 0xff, 0, 0, 0, 0});
  CHECK(bytes_of(b::stx(b::kDw, b::R0, 0, b::R1)) == Bytes{0x7b, 0x10, 0, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::st_imm(b::kH, b::R10, -2, 0)) == Bytes{0x6a, 0x0a, 0xfe, 0xff, 0, 0, 0, 0});
  CHECK(bytes_of(b::st_imm(b::kW, b::R10, -12, 7)) == Bytes{0x62, 0x0a, 0xf4, 0xff, 0x07, 0, 0, 0});
  CHECK(bytes_of(b::alu64_imm(b::kAdd, b::R4, 14)) == Bytes{0x07, 0x04, 0, 0, 0x0e, 0, 0, 0});
  CHECK(bytes_of(b::alu64_imm(b::kAdd, b::R2, -8)) ==
        Bytes{0x07, 0x02, 0, 0, 0xf8, 0xff, 0xff, 0xff});
  CHECK(bytes_of(b::alu64_imm(b::kAnd, b::R5, 0x0f)) == Bytes{0x57, 0x05, 0, 0, 0x0f, 0, 0, 0});
  CHECK(bytes_of(b::alu64_imm(b::kLsh, b::R5, 2)) == Bytes{0x67, 0x05, 0, 0, 0x02, 0, 0, 0});
  CHECK(bytes_of(b::alu64_reg(b::kAdd, b::R2, b::R5)) == Bytes{0x0f, 0x52, 0, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::jmp_reg(b::kJgt, b::R4, b::R3, 5)) == Bytes{0x2d, 0x34, 0x05, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::jmp_imm(b::kJne, b::R5, 0x81, 4)) == Bytes{0x55, 0x05, 0x04, 0, 0x81, 0, 0, 0});
  CHECK(bytes_of(b::jmp_imm(b::kJeq, b::R0, 0, 20)) == Bytes{0x15, 0x00, 0x14, 0, 0, 0, 0, 0});
  CHECK(bytes_of(b::jmp_imm(b::kJlt, b::R5, 5, 1)) == Bytes{0xa5, 0x05, 0x01, 0, 0x05, 0, 0, 0});
  CHECK(bytes_of(b::ja(-3)) == Bytes{0x05, 0x00, 0xfd, 0xff, 0, 0, 0, 0});
  CHECK(bytes_of(b::call(51)) == Bytes{0x85, 0x00, 0, 0, 0x33, 0, 0, 0});
  CHECK(bytes_of(b::exit_insn()) == Bytes{0x95, 0x00, 0, 0, 0, 0, 0, 0});
  const auto ld = b::ld_map_fd(b::R1, 7);
  CHECK(bytes_of(ld[0]) == Bytes{0x18, 0x11, 0, 0, 0x07, 0, 0, 0});
  CHECK(bytes_of(ld[1]) == Bytes{0, 0, 0, 0, 0, 0, 0, 0});
}

TEST_CASE("bpf_asm: labels resolve forward and backward") {
  constexpr auto prog = [] {
    b::Assembler<8> a;
    a.bind(0);
    a.emit(b::mov64_imm(b::R0, 0));
    a.jmp_imm(b::kJeq, b::R0, 1, 1);  // forward to label 1
    a.ja(0);                          // backward to label 0
    a.bind(1);
    a.emit(b::exit_insn());
    a.finish();
    return a;
  }();
  static_assert(prog.ok());
  static_assert(prog.size() == 4);
  static_assert(prog.insns()[1].off == 1);
  static_assert(prog.insns()[2].off == -3);

  b::Assembler<4> unbound;
  unbound.ja(3);
  CHECK_FALSE(unbound.finish());
  b::Assembler<1> full;
  full.emit(b::exit_insn());
  full.emit(b::exit_insn());
  CHECK_FALSE(full.ok());
  b::Assembler<4> twice;
  twice.bind(0);
  twice.bind(0);
  CHECK_FALSE(twice.ok());
}

// ---- the filter program -------------------------------------------------------------------------

TEST_CASE("program: structure") {
  constexpr xdp::Program p = xdp::build_program(kFakeFds);
  static_assert(p.ok);
  REQUIRE(p.size > 0);
  REQUIRE(p.size <= xdp::kProgramCapacity);
  const std::span<const b::Insn> insns(p.insns.data(), p.size);
  CHECK(insns.back() == b::exit_insn());

  std::vector<bool> second_half(insns.size(), false);
  for (std::size_t i = 0; i < insns.size(); ++i)
    if (b::is_ld_imm64(insns[i]) && i + 1 < insns.size()) second_half[i + 1] = true;

  int map_loads = 0;
  for (std::size_t i = 0; i < insns.size(); ++i) {
    const b::Insn& in = insns[i];
    INFO("insn " << i);
    if (second_half[i]) {
      CHECK(in == b::Insn{});
      continue;
    }
    const std::uint8_t cls = in.code & 0x07U;
    const std::uint8_t op = in.code & 0xF0U;
    CHECK(in.dst() <= 10);
    CHECK(in.src() <= 10);
    if (b::is_ld_imm64(in)) {
      CHECK(in.src() == b::kPseudoMapFd);
      CHECK((in.imm == kFakeFds.subs || in.imm == kFakeFds.xsks || in.imm == kFakeFds.fallback));
      ++map_loads;
    }
    // r10 is read-only.
    if (cls == b::kAlu64 || cls == b::kLdx || b::is_ld_imm64(in)) CHECK(in.dst() != b::R10);
    // Stack accesses stay inside the 512-byte frame.
    if ((cls == b::kStx || cls == b::kSt) && in.dst() == b::R10) {
      CHECK(in.off < 0);
      CHECK(in.off >= -512);
    }
    if (b::is_jump(in)) {
      const auto target = static_cast<std::ptrdiff_t>(i) + 1 + in.off;
      REQUIRE(target >= 0);
      REQUIRE(target < static_cast<std::ptrdiff_t>(insns.size()));
      CHECK_FALSE(second_half[static_cast<std::size_t>(target)]);
      CHECK(target > static_cast<std::ptrdiff_t>(i));  // no loops
    }
    if (cls == b::kJmp && op == b::kCall) {
      CHECK((in.imm == xdp::kHelperMapLookupElem || in.imm == xdp::kHelperRedirectMap));
      // A lookup's result is null-checked before anything else.
      if (in.imm == xdp::kHelperMapLookupElem) {
        REQUIRE(i + 1 < insns.size());
        const b::Insn& next = insns[i + 1];
        CHECK(next.code == (b::kJmp | b::kJeq | b::kK));
        CHECK(next.dst() == b::R0);
        CHECK(next.imm == 0);
      }
    }
  }
  CHECK(map_loads == 3);
  // Every instruction is reachable and every path ends in exit: walk all branches.
  std::vector<bool> seen(insns.size(), false);
  std::vector<std::size_t> todo{0};
  while (!todo.empty()) {
    const std::size_t i = todo.back();
    todo.pop_back();
    if (i >= insns.size() || seen[i]) continue;
    seen[i] = true;
    const b::Insn& in = insns[i];
    const std::uint8_t op = in.code & 0xF0U;
    if (b::is_ld_imm64(in)) {
      seen[i + 1] = true;
      todo.push_back(i + 2);
    } else if ((in.code & 0x07U) == b::kJmp && op == b::kExit) {
      continue;
    } else if (b::is_jump(in)) {
      todo.push_back(static_cast<std::size_t>(static_cast<std::ptrdiff_t>(i) + 1 + in.off));
      if (op != b::kJa) todo.push_back(i + 1);
    } else {
      REQUIRE_MESSAGE(i + 1 < insns.size(), "falls off the end at " << i);
      todo.push_back(i + 1);
    }
  }
  for (std::size_t i = 0; i < insns.size(); ++i) CHECK_MESSAGE(seen[i], "unreachable " << i);
}

TEST_CASE("program: interpreter verdicts agree with the parser") {
  constexpr xdp::Program p = xdp::build_program(kFakeFds);
  const std::span<const b::Insn> prog(p.insns.data(), p.size);
  std::size_t redirects = 0;
  for (const auto& c : test::frame_cases()) {
    // Every prefix too: the truncated frames exercise each bounds check.
    for (std::size_t n = 0; n <= c.frame.size(); ++n) {
      const auto frame = std::span<const std::byte>(c.frame).first(n);
      const UdpFrame parsed = parse_udp_frame(frame);
      const bool match = passes_xdp_filter(parsed.status) && test::key_matches(parsed);
      INFO(std::string(c.name) << ", " << n << " of " << c.frame.size() << " bytes");

      test::BpfEnv env = env_for(frame);
      env.xsks.insert(0);
      const test::BpfRun run = bpf_run(prog, env);
      REQUIRE_MESSAGE(run.ok, run.error);
      CHECK(run.r0 == static_cast<std::uint64_t>(match ? xdp::kRedirect : xdp::kPass));
      CHECK(env.fallback == 0);
      redirects += match ? 1 : 0;

      // No socket on the packet's queue: XDP_PASS, and the fallback counter counts matches.
      test::BpfEnv other = env_for(frame);
      other.xsks.insert(0);
      other.rx_queue = 1;
      const test::BpfRun fallback = bpf_run(prog, other);
      REQUIRE_MESSAGE(fallback.ok, fallback.error);
      CHECK(fallback.r0 == static_cast<std::uint64_t>(xdp::kPass));
      CHECK(other.fallback == (match ? 1U : 0U));
    }
  }
  CHECK(redirects > 10);
}

TEST_CASE("program: a subscription is keyed by group and port together") {
  constexpr xdp::Program p = xdp::build_program(kFakeFds);
  const std::span<const b::Insn> prog(p.insns.data(), p.size);
  FrameSpec s;
  const auto f = build_frame(s);
  test::BpfEnv env;
  env.packet = f;
  env.xsks.insert(0);
  env.subs.insert(test::key_bits({ip("239.10.0.1"), htons(31002), 0}));
  env.subs.insert(test::key_bits({ip("239.10.0.2"), htons(31001), 0}));
  const test::BpfRun run = bpf_run(prog, env);
  REQUIRE(run.ok);
  CHECK(run.r0 == static_cast<std::uint64_t>(xdp::kPass));
}

// ---- open() argument checks (no privileges needed to fail early) --------------------------------

TEST_CASE("source: open rejects bad configurations before touching the kernel") {
  XdpDatagramSource src;
  XdpConfig cfg;
  CHECK(src.open(cfg) == -EINVAL);  // no subscriptions
  cfg.subscriptions.push_back({"lo", ip("239.1.1.1"), 5000, 0});
  cfg.frame_count = 1000;
  CHECK(src.open(cfg) == -EINVAL);
  CHECK(src.error().find("frame_count") != std::string::npos);
  cfg.frame_count = 4096;
  cfg.frame_size = 3000;
  CHECK(src.open(cfg) == -EINVAL);
  cfg.frame_size = 4096;
  cfg.batch = 0;
  CHECK(src.open(cfg) == -EINVAL);
  cfg.batch = 64;
  if (xdp_kernel_error().empty() && xdp_capability_error().empty()) return;  // privileged run
  const int rc = src.open(cfg);
  CHECK((rc == -EPERM || rc == -ENOSYS));
  if (rc == -EPERM) {
    CHECK(src.error().find("setcap cap_net_admin,cap_net_raw,cap_bpf,cap_ipc_lock+ep") !=
          std::string::npos);
  }
  CHECK(src.fds().empty());
}

TEST_CASE("source: the kernel version check accepts this kernel or says why") {
  const std::string e = xdp_kernel_error();
  if (!e.empty()) CHECK(e.find("5.11") != std::string::npos);
}
