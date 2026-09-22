#pragma once
// eBPF instruction encoding and a fixed-capacity assembler with forward labels, enough to write
// the AF_XDP filter program (xdp_program.hpp) as C++ instead of carrying a BPF compiler. Encodings
// follow the kernel's instruction set (Documentation/bpf/standardization/instruction-set.rst).
// Everything is constexpr; nothing allocates.
#include <array>
#include <cstddef>
#include <cstdint>

namespace fastmm::net::bpf {

// struct bpf_insn: opcode, dst_reg:4 | src_reg:4 (low nibble dst on little-endian bitfields),
// signed 16-bit offset, signed 32-bit immediate.
struct Insn {
  std::uint8_t code = 0;
  std::uint8_t regs = 0;
  std::int16_t off = 0;
  std::int32_t imm = 0;

  [[nodiscard]] constexpr std::uint8_t dst() const noexcept { return regs & 0x0FU; }
  [[nodiscard]] constexpr std::uint8_t src() const noexcept { return regs >> 4U; }
  friend constexpr bool operator==(const Insn&, const Insn&) = default;
};
static_assert(sizeof(Insn) == 8);

enum Reg : std::uint8_t { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10 };

// Instruction classes.
inline constexpr std::uint8_t kLd = 0x00;
inline constexpr std::uint8_t kLdx = 0x01;
inline constexpr std::uint8_t kSt = 0x02;
inline constexpr std::uint8_t kStx = 0x03;
inline constexpr std::uint8_t kAlu = 0x04;
inline constexpr std::uint8_t kJmp = 0x05;
inline constexpr std::uint8_t kAlu64 = 0x07;
// Load/store sizes and modes.
inline constexpr std::uint8_t kW = 0x00;
inline constexpr std::uint8_t kH = 0x08;
inline constexpr std::uint8_t kB = 0x10;
inline constexpr std::uint8_t kDw = 0x18;
inline constexpr std::uint8_t kImm = 0x00;
inline constexpr std::uint8_t kMem = 0x60;
// Operand source.
inline constexpr std::uint8_t kK = 0x00;
inline constexpr std::uint8_t kX = 0x08;
// ALU operations.
inline constexpr std::uint8_t kAdd = 0x00;
inline constexpr std::uint8_t kSub = 0x10;
inline constexpr std::uint8_t kOr = 0x40;
inline constexpr std::uint8_t kAnd = 0x50;
inline constexpr std::uint8_t kLsh = 0x60;
inline constexpr std::uint8_t kRsh = 0x70;
inline constexpr std::uint8_t kMov = 0xB0;
// Jump operations.
inline constexpr std::uint8_t kJa = 0x00;
inline constexpr std::uint8_t kJeq = 0x10;
inline constexpr std::uint8_t kJgt = 0x20;
inline constexpr std::uint8_t kJge = 0x30;
inline constexpr std::uint8_t kJne = 0x50;
inline constexpr std::uint8_t kJlt = 0xA0;
inline constexpr std::uint8_t kJle = 0xB0;
inline constexpr std::uint8_t kCall = 0x80;
inline constexpr std::uint8_t kExit = 0x90;
// src_reg of the first half of a 64-bit immediate load: imm is a map fd.
inline constexpr std::uint8_t kPseudoMapFd = 1;

[[nodiscard]] constexpr Insn make(std::uint8_t code,
                                  std::uint8_t dst,
                                  std::uint8_t src,
                                  std::int16_t off,
                                  std::int32_t imm) noexcept {
  return Insn{code, static_cast<std::uint8_t>((dst & 0x0FU) | ((src & 0x0FU) << 4U)), off, imm};
}

[[nodiscard]] constexpr Insn alu64_imm(std::uint8_t op, Reg dst, std::int32_t imm) noexcept {
  return make(kAlu64 | op | kK, dst, 0, 0, imm);
}
[[nodiscard]] constexpr Insn alu64_reg(std::uint8_t op, Reg dst, Reg src) noexcept {
  return make(kAlu64 | op | kX, dst, src, 0, 0);
}
[[nodiscard]] constexpr Insn mov64_imm(Reg dst, std::int32_t imm) noexcept {
  return alu64_imm(kMov, dst, imm);
}
[[nodiscard]] constexpr Insn mov64_reg(Reg dst, Reg src) noexcept {
  return alu64_reg(kMov, dst, src);
}
// dst = *(size *)(src + off)
[[nodiscard]] constexpr Insn ldx(std::uint8_t size, Reg dst, Reg src, std::int16_t off) noexcept {
  return make(kLdx | kMem | size, dst, src, off, 0);
}
// *(size *)(dst + off) = src
[[nodiscard]] constexpr Insn stx(std::uint8_t size, Reg dst, std::int16_t off, Reg src) noexcept {
  return make(kStx | kMem | size, dst, src, off, 0);
}
// *(size *)(dst + off) = imm
[[nodiscard]] constexpr Insn st_imm(std::uint8_t size,
                                    Reg dst,
                                    std::int16_t off,
                                    std::int32_t imm) noexcept {
  return make(kSt | kMem | size, dst, 0, off, imm);
}
// if (dst op imm) pc += off
[[nodiscard]] constexpr Insn jmp_imm(std::uint8_t op,
                                     Reg dst,
                                     std::int32_t imm,
                                     std::int16_t off) noexcept {
  return make(kJmp | op | kK, dst, 0, off, imm);
}
// if (dst op src) pc += off
[[nodiscard]] constexpr Insn jmp_reg(std::uint8_t op, Reg dst, Reg src, std::int16_t off) noexcept {
  return make(kJmp | op | kX, dst, src, off, 0);
}
[[nodiscard]] constexpr Insn ja(std::int16_t off) noexcept {
  return make(kJmp | kJa, 0, 0, off, 0);
}
[[nodiscard]] constexpr Insn call(std::int32_t helper) noexcept {
  return make(kJmp | kCall, 0, 0, 0, helper);
}
[[nodiscard]] constexpr Insn exit_insn() noexcept {
  return make(kJmp | kExit, 0, 0, 0, 0);
}
// dst = map (by fd): two instruction slots, the second carrying the upper 32 bits (zero).
[[nodiscard]] constexpr std::array<Insn, 2> ld_map_fd(Reg dst, int fd) noexcept {
  return {make(kLd | kDw | kImm, dst, kPseudoMapFd, 0, fd), Insn{}};
}

[[nodiscard]] constexpr bool is_ld_imm64(const Insn& i) noexcept {
  return i.code == (kLd | kDw | kImm);
}
[[nodiscard]] constexpr bool is_jump(const Insn& i) noexcept {
  const std::uint8_t op = i.code & 0xF0U;
  return (i.code & 0x07U) == kJmp && op != kCall && op != kExit;
}

// Assembler over a fixed array. Jumps name a label; bind() places it; finish() patches offsets
// and reports unbound labels or out-of-range offsets through ok().
template <std::size_t Capacity, std::size_t Labels = 16>
class Assembler {
 public:
  using Label = std::size_t;

  constexpr void emit(Insn i) noexcept {
    if (size_ >= Capacity) {
      ok_ = false;
      return;
    }
    insns_[size_++] = i;
  }
  constexpr void emit(const std::array<Insn, 2>& pair) noexcept {
    emit(pair[0]);
    emit(pair[1]);
  }
  constexpr void jmp_imm(std::uint8_t op, Reg dst, std::int32_t imm, Label to) noexcept {
    fixup(to);
    emit(bpf::jmp_imm(op, dst, imm, 0));
  }
  constexpr void jmp_reg(std::uint8_t op, Reg dst, Reg src, Label to) noexcept {
    fixup(to);
    emit(bpf::jmp_reg(op, dst, src, 0));
  }
  constexpr void ja(Label to) noexcept {
    fixup(to);
    emit(bpf::ja(0));
  }
  constexpr void bind(Label l) noexcept {
    if (l >= Labels || bound_[l] >= 0) {
      ok_ = false;
      return;
    }
    bound_[l] = static_cast<std::ptrdiff_t>(size_);
  }

  // Resolves jump offsets. Returns ok().
  constexpr bool finish() noexcept {
    for (std::size_t k = 0; k < fixups_; ++k) {
      const auto at = fixup_at_[k];
      const auto target = bound_[fixup_label_[k]];
      if (target < 0) {
        ok_ = false;
        continue;
      }
      const std::ptrdiff_t rel = target - static_cast<std::ptrdiff_t>(at) - 1;
      if (rel < INT16_MIN || rel > INT16_MAX) {
        ok_ = false;
        continue;
      }
      insns_[at].off = static_cast<std::int16_t>(rel);
    }
    fixups_ = 0;
    return ok_;
  }

  [[nodiscard]] constexpr bool ok() const noexcept { return ok_; }
  [[nodiscard]] constexpr std::size_t size() const noexcept { return size_; }
  [[nodiscard]] constexpr const std::array<Insn, Capacity>& insns() const noexcept {
    return insns_;
  }

 private:
  constexpr void fixup(Label to) noexcept {
    if (to >= Labels || fixups_ >= kMaxFixups || size_ >= Capacity) {
      ok_ = false;
      return;
    }
    fixup_at_[fixups_] = size_;
    fixup_label_[fixups_] = to;
    ++fixups_;
  }

  static constexpr std::size_t kMaxFixups = Capacity;
  std::array<Insn, Capacity> insns_{};
  std::size_t size_ = 0;
  std::array<std::ptrdiff_t, Labels> bound_ = filled(-1);
  std::array<std::size_t, kMaxFixups> fixup_at_{};
  std::array<std::size_t, kMaxFixups> fixup_label_{};
  std::size_t fixups_ = 0;
  bool ok_ = true;

  static constexpr std::array<std::ptrdiff_t, Labels> filled(std::ptrdiff_t v) noexcept {
    std::array<std::ptrdiff_t, Labels> a{};
    for (auto& x : a) x = v;
    return a;
  }
};

}  // namespace fastmm::net::bpf
