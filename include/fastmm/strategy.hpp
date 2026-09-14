#pragma once
// Everything a strategy header needs (ADR-0012):
//
//   fixed point and literals   Price, Qty, Notional, Ratio, 100.25_px, 0.01_qty, 5_bps
//   instruments and time       Instrument, InstrumentId, Timestamp, Duration, milliseconds()
//   messages and books         Level, TradeMsg, BookTickerMsg, ConnectionStateMsg, BookView
//   quotes and helpers         DesiredQuotes, mid, microprice, inventory_allows, keep_passive, ...
//   orders                     NewOrderRequest, OmsUpdate, Order
//   parameters                 FASTMM_PARAMS, FASTMM_PARAM, FASTMM_PARAM_BPS, FASTMM_PARAM_MS
//   hooks                      Fill, verify_strategy, StrategyBase, StrategyLike
//   logging                    FASTMM_LOG_INFO, FASTMM_LOG_WARN, ...
//
//   #include "fastmm/strategy.hpp"
//   using namespace fastmm::literals;   // or `using namespace fastmm;`, which includes them
#include "fastmm/core/book/book_view.hpp"
#include "fastmm/core/enums.hpp"
#include "fastmm/core/fixed_point.hpp"
#include "fastmm/core/instrument.hpp"
#include "fastmm/core/log.hpp"
#include "fastmm/core/messages.hpp"
#include "fastmm/core/oms.hpp"
#include "fastmm/core/quote_manager.hpp"
#include "fastmm/core/time.hpp"
#include "fastmm/strategies/hooks.hpp"
#include "fastmm/strategies/params.hpp"
#include "fastmm/strategies/quoting.hpp"
#include "fastmm/strategies/strategy.hpp"
