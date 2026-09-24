// DpdkDatagramSource without DPDK in the build: open() fails with -ENOTSUP. The real
// implementation is src/dpdk/dpdk_datagram_source.cpp (FASTMM_WITH_DPDK=ON).
#ifndef FASTMM_HAS_DPDK

#include "fastmm/net/dpdk_datagram_source.hpp"

#include <cerrno>

namespace fastmm::net {

bool dpdk_available() noexcept {
  return false;
}

int DpdkDatagramSource::fail(int err, std::string msg) {
  error_ = std::move(msg);
  return err;
}

int DpdkDatagramSource::open(const DpdkConfig&) {
  return fail(-ENOTSUP, "built without DPDK (configure with -DFASTMM_WITH_DPDK=ON)");
}

int DpdkDatagramSource::open_exception(const DpdkConfig&, unsigned) {
  return -ENOTSUP;
}

void DpdkDatagramSource::close() noexcept {}

std::uint32_t DpdkDatagramSource::rx_burst() noexcept {
  return 0;
}

void DpdkDatagramSource::free_burst(std::uint32_t) noexcept {}

void DpdkDatagramSource::divert(std::uint32_t) noexcept {}

void DpdkDatagramSource::service_exception() noexcept {}

bool DpdkDatagramSource::send_frame(std::span<const std::byte>) noexcept {
  return false;
}

int DpdkDatagramSource::refresh_stats() noexcept {
  return -ENOTSUP;
}

}  // namespace fastmm::net

#endif
