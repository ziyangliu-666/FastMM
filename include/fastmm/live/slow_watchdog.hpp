#pragma once
// slow_tier_watchdog(): the LiveOptions::watchdog of a session whose strategy has a Python slow
// tier (ADR-0013, section 4). fastmm_live._live installs it; the control thread calls it every 50
// ms without the GIL.
//
// A call reports the channel's failure (SlowChannel::check, which also records a call past its
// timeout) and, when a slow thread is watched, records SlowFailure::ThreadExited once that thread
// no longer exists, even if it ended without running any Python code. The first non-empty result is
// a log line naming the cause; run_live then stops the session with kExitSlowTier.
#include "fastmm/strategies/slow_channel.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace fastmm::live {

// The log line of a failure (empty for SlowFailure::None), at most kLogMaxStrBytes long.
[[nodiscard]] std::string slow_failure_cause(SlowFailure f);

// True while the thread `tid` of this process exists (tid <= 0: not watched, true).
[[nodiscard]] bool thread_alive(std::int64_t tid) noexcept;

// `slow_tid` is the Linux thread id of the slow thread (threading.get_native_id()), or 0 when the
// strategy has no slow thread.
[[nodiscard]] std::function<std::string()> slow_tier_watchdog(std::shared_ptr<SlowChannel> channel,
                                                              std::int64_t slow_tid);

}  // namespace fastmm::live
