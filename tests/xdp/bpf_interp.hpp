#pragma once
// A small eBPF interpreter for the XDP filter program, so its logic can be tested without
// privileges. It covers the instructions build_program() emits and is strict where the verifier
// is: registers are typed (scalar, context, packet, packet end, stack, map, map value); reading an
// unset register, a packet byte outside [data, data_end), an unwritten stack byte, or a helper
// argument of the wrong type fails the run. It only checks the path a given frame takes, so the
// kernel verifier (tests/xdp/xdp_privileged_test.cpp) is still the authority.
#include "fastmm/net/bpf_asm.hpp"
#include "fastmm/net/xdp_program.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <set>
#include <span>
#include <string>

namespace fastmm::net::test {

struct BpfEnv {
  static constexpr int kSubsFd = 101;
  static constexpr int kXsksFd = 102;
  static constexpr int kFallbackFd = 103;

  std::span<const std::byte> packet;
  std::uint32_t rx_queue = 0;
  std::set<std::uint64_t> subs;  // Key bytes as a u64
  std::set<std::uint32_t> xsks;  // queues with a socket
  std::uint64_t fallback = 0;    // per-CPU counter (one CPU)
};

struct BpfRun {
  bool ok = false;
  std::string error;
  std::uint64_t r0 = 0;
  std::size_t steps = 0;
};

inline std::uint64_t key_bits(const xdp::Key& k) {
  std::uint64_t v = 0;
  std::memcpy(&v, &k, sizeof(v));
  return v;
}

inline BpfRun bpf_run(std::span<const bpf::Insn> prog, BpfEnv& env) {
  using namespace bpf;
  enum class K : std::uint8_t { Unset, Scalar, Ctx, Pkt, PktEnd, Stack, Map, MapValue };
  struct V {
    K k = K::Unset;
    std::int64_t v = 0;  // scalar value, pointer offset or map fd
  };
  std::array<V, 11> r{};
  std::array<std::uint8_t, 512> stack{};
  std::array<bool, 512> init{};
  r[1] = {K::Ctx, 0};
  r[10] = {K::Stack, 0};
  BpfRun out;
  const auto fail = [&out](std::string e) {
    out.ok = false;
    out.error = std::move(e);
    return out;
  };
  const auto stack_at = [](std::int64_t off, std::size_t size) -> std::int64_t {
    const std::int64_t at = 512 + off;
    return (off < 0 && at >= 0 && at + static_cast<std::int64_t>(size) <= 512) ? at : -1;
  };
  const auto sz = [](std::uint8_t code) -> std::size_t {
    switch (code & 0x18U) {
      case kW:
        return 4;
      case kH:
        return 2;
      case kB:
        return 1;
      default:
        return 8;
    }
  };

  std::size_t pc = 0;
  for (;;) {
    if (++out.steps > 4096) return fail("step limit");
    if (pc >= prog.size()) return fail("fell off the end");
    const Insn in = prog[pc];
    const std::uint8_t cls = in.code & 0x07U;
    const std::uint8_t dst = in.dst();
    const std::uint8_t src = in.src();
    if (dst > 10 || src > 10) return fail("bad register");
    const auto read = [&](std::uint8_t reg) -> const V* {
      return r[reg].k == K::Unset ? nullptr : &r[reg];
    };

    if (is_ld_imm64(in)) {
      if (pc + 1 >= prog.size()) return fail("truncated ld_imm64");
      if (src != kPseudoMapFd) return fail("ld_imm64 without a map fd");
      if (dst == 10) return fail("write to r10");
      r[dst] = {K::Map, in.imm};
      pc += 2;
      continue;
    }
    if (cls == kAlu64) {
      if (dst == 10) return fail("write to r10");
      const std::uint8_t op = in.code & 0xF0U;
      V operand{K::Scalar, in.imm};
      if ((in.code & kX) != 0) {
        const V* s = read(src);
        if (s == nullptr) return fail("read of unset r" + std::to_string(src));
        operand = *s;
      }
      if (op == kMov) {
        r[dst] = operand;
        ++pc;
        continue;
      }
      const V* d = read(dst);
      if (d == nullptr) return fail("read of unset r" + std::to_string(dst));
      V res = *d;
      if (d->k != K::Scalar) {
        if (op != kAdd || operand.k != K::Scalar || (d->k != K::Pkt && d->k != K::Stack))
          return fail("pointer arithmetic other than ptr += scalar");
        res.v += operand.v;
      } else {
        if (operand.k != K::Scalar) return fail("scalar op pointer");
        const auto a = static_cast<std::uint64_t>(d->v);
        const auto b = static_cast<std::uint64_t>(operand.v);
        std::uint64_t x = 0;
        switch (op) {
          case kAdd:
            x = a + b;
            break;
          case kSub:
            x = a - b;
            break;
          case kAnd:
            x = a & b;
            break;
          case kOr:
            x = a | b;
            break;
          case kLsh:
            x = a << (b & 63U);
            break;
          case kRsh:
            x = a >> (b & 63U);
            break;
          default:
            return fail("unsupported alu op");
        }
        res.v = static_cast<std::int64_t>(x);
      }
      r[dst] = res;
      ++pc;
      continue;
    }
    if (cls == kLdx) {
      if ((in.code & 0xE0U) != kMem) return fail("unsupported ldx mode");
      if (dst == 10) return fail("write to r10");
      const V* s = read(src);
      if (s == nullptr) return fail("read of unset r" + std::to_string(src));
      const std::size_t n = sz(in.code);
      const std::int64_t off = s->v + in.off;
      std::uint64_t val = 0;
      if (s->k == K::Ctx) {
        if (n != 4) return fail("ctx access must be 4 bytes");
        if (off == xdp::kMdData) {
          r[dst] = {K::Pkt, 0};
        } else if (off == xdp::kMdDataEnd) {
          r[dst] = {K::PktEnd, 0};
        } else if (off == xdp::kMdRxQueueIndex) {
          r[dst] = {K::Scalar, env.rx_queue};
        } else {
          return fail("unexpected ctx offset");
        }
        ++pc;
        continue;
      }
      if (s->k == K::Pkt) {
        if (off < 0 ||
            off + static_cast<std::int64_t>(n) > static_cast<std::int64_t>(env.packet.size()))
          return fail("packet read out of bounds at " + std::to_string(off) + " (pc " +
                      std::to_string(pc) + ", size " + std::to_string(env.packet.size()) + ")");
        std::memcpy(&val, env.packet.data() + off, n);
      } else if (s->k == K::Stack) {
        const std::int64_t at = stack_at(off, n);
        if (at < 0) return fail("stack read out of bounds");
        for (std::size_t k = 0; k < n; ++k)
          if (!init[static_cast<std::size_t>(at) + k]) return fail("read of unwritten stack");
        std::memcpy(&val, stack.data() + at, n);
      } else if (s->k == K::MapValue) {  // v is the map fd; only the fallback value is read
        if (s->v != BpfEnv::kFallbackFd || in.off != 0 || n != 8) return fail("map value access");
        val = env.fallback;
      } else {
        return fail("load through a non-pointer");
      }
      r[dst] = {K::Scalar, static_cast<std::int64_t>(val)};
      ++pc;
      continue;
    }
    if (cls == kStx || cls == kSt) {
      if ((in.code & 0xE0U) != kMem) return fail("unsupported store mode");
      const V* d = read(dst);
      if (d == nullptr) return fail("read of unset r" + std::to_string(dst));
      std::uint64_t val = static_cast<std::uint64_t>(static_cast<std::int64_t>(in.imm));
      if (cls == kStx) {
        const V* s = read(src);
        if (s == nullptr) return fail("read of unset r" + std::to_string(src));
        if (s->k != K::Scalar) return fail("storing a pointer");
        val = static_cast<std::uint64_t>(s->v);
      }
      const std::size_t n = sz(in.code);
      const std::int64_t off = d->v + in.off;
      if (d->k == K::Stack) {
        const std::int64_t at = stack_at(off, n);
        if (at < 0) return fail("stack write out of bounds");
        std::memcpy(stack.data() + at, &val, n);
        for (std::size_t k = 0; k < n; ++k) init[static_cast<std::size_t>(at) + k] = true;
      } else if (d->k == K::MapValue) {
        if (d->v != BpfEnv::kFallbackFd || in.off != 0 || n != 8) return fail("map value store");
        env.fallback = val;
      } else {
        return fail("store through a non-writable pointer");
      }
      ++pc;
      continue;
    }
    if (cls == kJmp) {
      const std::uint8_t op = in.code & 0xF0U;
      if (op == kExit) {
        const V* v = read(0);
        if (v == nullptr || v->k != K::Scalar) return fail("exit without a scalar r0");
        out.ok = true;
        out.r0 = static_cast<std::uint64_t>(v->v);
        return out;
      }
      if (op == kCall) {
        const V* m = read(1);
        if (m == nullptr || m->k != K::Map) return fail("helper r1 is not a map");
        if (in.imm == xdp::kHelperMapLookupElem) {
          const V* key = read(2);
          if (key == nullptr || key->k != K::Stack) return fail("lookup key is not on the stack");
          const std::size_t ks = m->v == BpfEnv::kSubsFd ? 8 : 4;
          const std::int64_t at = stack_at(key->v, ks);
          if (at < 0) return fail("lookup key out of the stack");
          for (std::size_t k = 0; k < ks; ++k)
            if (!init[static_cast<std::size_t>(at) + k]) return fail("lookup key not written");
          if (m->v == BpfEnv::kSubsFd) {
            std::uint64_t kb = 0;
            std::memcpy(&kb, stack.data() + at, 8);
            r[0] = env.subs.count(kb) != 0 ? V{K::MapValue, BpfEnv::kSubsFd} : V{K::Scalar, 0};
          } else if (m->v == BpfEnv::kFallbackFd) {
            std::uint32_t kb = 0;
            std::memcpy(&kb, stack.data() + at, 4);
            r[0] = kb == 0 ? V{K::MapValue, BpfEnv::kFallbackFd} : V{K::Scalar, 0};
          } else {
            return fail("lookup in an unexpected map");
          }
        } else if (in.imm == xdp::kHelperRedirectMap) {
          if (m->v != BpfEnv::kXsksFd) return fail("redirect_map on the wrong map");
          const V* q = read(2);
          const V* flags = read(3);
          if (q == nullptr || q->k != K::Scalar || flags == nullptr || flags->k != K::Scalar)
            return fail("redirect_map arguments");
          const bool hit = env.xsks.count(static_cast<std::uint32_t>(q->v)) != 0;
          r[0] = {K::Scalar, hit ? xdp::kRedirect : flags->v};
        } else {
          return fail("unexpected helper " + std::to_string(in.imm));
        }
        for (int k = 1; k <= 5; ++k) r[static_cast<std::size_t>(k)] = {};
        ++pc;
        continue;
      }
      bool taken = true;
      if (op != kJa) {
        const V* a = read(dst);
        if (a == nullptr) return fail("read of unset r" + std::to_string(dst));
        V b{K::Scalar, in.imm};
        if ((in.code & kX) != 0) {
          const V* s = read(src);
          if (s == nullptr) return fail("read of unset r" + std::to_string(src));
          b = *s;
        }
        std::uint64_t x = 0;
        std::uint64_t y = 0;
        const auto pos = [&env](const V& v) -> std::int64_t {
          return v.k == K::PktEnd ? static_cast<std::int64_t>(env.packet.size()) + v.v : v.v;
        };
        if ((a->k == K::Pkt || a->k == K::PktEnd) && (b.k == K::Pkt || b.k == K::PktEnd)) {
          x = static_cast<std::uint64_t>(pos(*a));
          y = static_cast<std::uint64_t>(pos(b));
        } else if (a->k == K::MapValue && b.k == K::Scalar && b.v == 0) {
          x = 1;  // non-null
          y = 0;
        } else if (a->k == K::Scalar && b.k == K::Scalar) {
          x = static_cast<std::uint64_t>(a->v);
          y = static_cast<std::uint64_t>(b.v);
        } else {
          return fail("comparison of incompatible types");
        }
        switch (op) {
          case kJeq:
            taken = x == y;
            break;
          case kJne:
            taken = x != y;
            break;
          case kJgt:
            taken = x > y;
            break;
          case kJge:
            taken = x >= y;
            break;
          case kJlt:
            taken = x < y;
            break;
          case kJle:
            taken = x <= y;
            break;
          default:
            return fail("unsupported jump");
        }
      }
      pc = taken ? static_cast<std::size_t>(static_cast<std::int64_t>(pc) + 1 + in.off) : pc + 1;
      continue;
    }
    return fail("unsupported instruction class");
  }
}

}  // namespace fastmm::net::test
