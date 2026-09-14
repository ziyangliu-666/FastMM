#pragma once
// FIX 4.4 codec (plan 7): umbrella header. See docs/reference/codecs/fix.md.
//
//   FixFramer    byte stream -> one message per frame           (codecs::Framer)
//   FixView      zero-copy validating field index + typed getters
//   FixBuilder   message writer with BodyLength/CheckSum backfill
//   FixSession   Logon/Heartbeat/TestRequest/Resend/SequenceReset/Logout (codecs::SessionLayer)
//   MessageStore bounded append-only store of sent application messages
//   FixDecoder   ExecutionReport / OrderCancelReject / W / X -> engine events (codecs::Decoder)
//   FixEncoder   OrderCommand -> D / F / G                      (codecs::Encoder)
#include "fastmm/codecs/codec.hpp"
#include "fastmm/codecs/fix/fix_builder.hpp"
#include "fastmm/codecs/fix/fix_decoder.hpp"
#include "fastmm/codecs/fix/fix_encoder.hpp"
#include "fastmm/codecs/fix/fix_framer.hpp"
#include "fastmm/codecs/fix/fix_session.hpp"
#include "fastmm/codecs/fix/fix_symbols.hpp"
#include "fastmm/codecs/fix/fix_tags.hpp"
#include "fastmm/codecs/fix/fix_time.hpp"
#include "fastmm/codecs/fix/fix_view.hpp"
#include "fastmm/codecs/fix/message_store.hpp"

namespace fastmm::codecs::fix {

static_assert(Framer<FixFramer>);
static_assert(SessionLayer<FixSession>);
static_assert(Decoder<FixDecoder>);
static_assert(Encoder<FixEncoder>);

}  // namespace fastmm::codecs::fix
