// TotalView-ITCH 5.0 decoder (see itch_decoder.hpp for the message -> event mapping).
#include "fastmm/codecs/itch/itch_decoder.hpp"

#include "fastmm/core/config_macros.hpp"

#include <algorithm>

namespace fastmm::codecs::itch {

ItchDecoder::ItchDecoder(VenueId venue) : venue_(venue), locate_(new InstrumentId[kLocateSlots]) {}

bool ItchDecoder::add_symbol(std::string_view symbol, InstrumentId id) noexcept {
  if (symbol.empty() || symbol.size() > sizeof(StockDirectory::stock) || !id.valid()) return false;
  return symbols_.assign(nasdaq::symbol_key(symbol), id) != nullptr;
}

void ItchDecoder::clear_locates() noexcept {
  std::fill_n(locate_.get(), kLocateSlots, InstrumentId{});
}

bool ItchDecoder::lookup(const MessageHeader& h, InstrumentId& out) noexcept {
  out = locate_[h.stock_locate.get()];
  if (FASTMM_LIKELY(out.valid())) return true;
  ++stats_.unknown_locate;
  return false;
}

ParseStatus ItchDecoder::decode(const FrameView& frame,
                                std::int64_t rx_ts,
                                venues::EventSink& sink) noexcept {
  return decode_into(frame, rx_ts, sink);
}

template ParseStatus ItchDecoder::decode_into(const FrameView&,
                                              std::int64_t,
                                              venues::EventSink&) noexcept;
template ParseStatus ItchDecoder::decode_into(const FrameView&,
                                              std::int64_t,
                                              ScratchSink&) noexcept;

}  // namespace fastmm::codecs::itch
