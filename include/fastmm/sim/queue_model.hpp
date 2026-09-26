#pragma once
// The queue position model lives in core (core/queue_model.hpp): the simulator's l2_queue fill
// model and the engine's live estimate share it.
#include "fastmm/core/queue_model.hpp"

namespace fastmm::sim {

using fastmm::kMaxQueuedOrders;
using fastmm::level_qty;
using fastmm::queue_after_level_change;
using fastmm::queue_after_trade;
using fastmm::queue_apply_book;
using fastmm::QueuedOrder;
using fastmm::QueuePositionModel;

}  // namespace fastmm::sim
