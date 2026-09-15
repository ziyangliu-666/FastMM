/* C ABI between the engine thread and a compiled Python hot hook (ADR-0013, section 1).
 *
 * A hook is
 *
 *     int32_t hook(uint8_t* self, fastmm_hot_ctx* ctx, fastmm_hot_book* book);
 *
 * `self` is the instrument's record: the parameter block (copied by the engine before every call)
 * followed by the State fields (kept between calls). Its layout is generated per strategy class.
 * The engine fills the inputs of `ctx` and `book` and resets the outputs of `ctx` before every
 * call; the hook writes intents into the outputs and returns a FASTMM_HOT_* status.
 *
 * fastmm/_hot/abi.py mirrors both structs as numpy dtypes and checks them at import against the
 * offsets fastmm._core reports. Any change to a struct or constant increments
 * FASTMM_HOT_ABI_VERSION.
 */
#ifndef FASTMM_STRATEGIES_HOT_ABI_H
#define FASTMM_STRATEGIES_HOT_ABI_H

#include <stddef.h>
#include <stdint.h>

#define FASTMM_HOT_ABI_VERSION 1

#define FASTMM_HOT_BOOK_DEPTH 10  /* book levels per side a hook can read */
#define FASTMM_HOT_QUOTE_LEVELS 8 /* quote levels per side a hook can write (kMaxQuoteLevels) */

/* Hook status. EXCEPTION: the generated wrapper caught an exception. FAILED: the hook called
 * ctx.fail(code), and ctx->fail_code holds the code. BAD_VALUE (set by the engine): a float price
 * or quantity was not finite or out of range, or a quantity was negative. */
#define FASTMM_HOT_OK 0
#define FASTMM_HOT_EXCEPTION 1
#define FASTMM_HOT_FAILED 2
#define FASTMM_HOT_BAD_VALUE 3

/* ctx->action. QUOTE: set_quotes with the levels in ctx (none cancels the quotes). PULL:
 * pull_quotes. */
#define FASTMM_HOT_ACTION_NONE 0
#define FASTMM_HOT_ACTION_QUOTE 1
#define FASTMM_HOT_ACTION_PULL 2

/* ctx->flags, applied after the levels are converted, in this order */
#define FASTMM_HOT_FLAG_UNCROSS 1      /* DesiredQuotes::uncross(tick) */
#define FASTMM_HOT_FLAG_KEEP_PASSIVE 2 /* keep_passive(q, best_bid, best_ask, tick) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fastmm_hot_ctx {
  /* inputs */
  int64_t now_ns;
  int32_t instrument;
  uint8_t quoting_enabled; /* quoting is enabled and the instrument's venue is not killed */
  uint8_t connected;       /* on_connection: the venue's connection is live */
  uint8_t fill_side;       /* on_fill: 0 buy, 1 sell */
  uint8_t fill_maker;      /* on_fill: 1 when the fill added liquidity */
  double tick;
  double lot;
  double min_qty;
  double position;
  int64_t tick_raw;
  int64_t lot_raw;
  int64_t min_qty_raw;
  int64_t position_raw;
  double fill_price;
  double fill_qty;
  int64_t fill_price_raw;
  int64_t fill_qty_raw;
  /* outputs */
  int32_t action;
  int32_t flags;
  int32_t n_bids;
  int32_t n_asks;
  int32_t status;
  int32_t fail_code;
  double bid_px[FASTMM_HOT_QUOTE_LEVELS];
  double bid_qty[FASTMM_HOT_QUOTE_LEVELS];
  double ask_px[FASTMM_HOT_QUOTE_LEVELS];
  double ask_qty[FASTMM_HOT_QUOTE_LEVELS];
  int64_t bid_px_raw[FASTMM_HOT_QUOTE_LEVELS];
  int64_t bid_qty_raw[FASTMM_HOT_QUOTE_LEVELS];
  int64_t ask_px_raw[FASTMM_HOT_QUOTE_LEVELS];
  int64_t ask_qty_raw[FASTMM_HOT_QUOTE_LEVELS];
  uint8_t bid_is_raw[FASTMM_HOT_QUOTE_LEVELS]; /* 1: the level is in bid_px_raw / bid_qty_raw */
  uint8_t ask_is_raw[FASTMM_HOT_QUOTE_LEVELS];
} fastmm_hot_ctx;

typedef struct fastmm_hot_book {
  int64_t ts_ns; /* last update, engine time (0: never updated) */
  uint8_t valid;
  int32_t n_bids; /* levels present in bid_px ..., at most FASTMM_HOT_BOOK_DEPTH */
  int32_t n_asks;
  double mid;
  double best_bid;
  double best_ask;
  double best_bid_qty;
  double best_ask_qty;
  int64_t mid_raw;
  int64_t best_bid_raw;
  int64_t best_ask_raw;
  int64_t best_bid_qty_raw;
  int64_t best_ask_qty_raw;
  double bid_px[FASTMM_HOT_BOOK_DEPTH];
  double bid_qty[FASTMM_HOT_BOOK_DEPTH];
  double ask_px[FASTMM_HOT_BOOK_DEPTH];
  double ask_qty[FASTMM_HOT_BOOK_DEPTH];
  int64_t bid_px_raw[FASTMM_HOT_BOOK_DEPTH];
  int64_t bid_qty_raw[FASTMM_HOT_BOOK_DEPTH];
  int64_t ask_px_raw[FASTMM_HOT_BOOK_DEPTH];
  int64_t ask_qty_raw[FASTMM_HOT_BOOK_DEPTH];
} fastmm_hot_book;

typedef int32_t (*fastmm_hot_fn)(uint8_t* self, fastmm_hot_ctx* ctx, fastmm_hot_book* book);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#define FASTMM_HOT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define FASTMM_HOT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

/* Layout of ABI version 1 (x86-64 System V). */
FASTMM_HOT_STATIC_ASSERT(sizeof(fastmm_hot_ctx) == 664, "fastmm_hot_ctx size");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_ctx, tick) == 16, "fastmm_hot_ctx.tick");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_ctx, fill_price) == 80, "fastmm_hot_ctx.fill_price");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_ctx, action) == 112, "fastmm_hot_ctx.action");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_ctx, bid_px) == 136, "fastmm_hot_ctx.bid_px");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_ctx, bid_is_raw) == 648, "fastmm_hot_ctx.bid_is_raw");
FASTMM_HOT_STATIC_ASSERT(sizeof(fastmm_hot_book) == 744, "fastmm_hot_book size");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_book, n_bids) == 12, "fastmm_hot_book.n_bids");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_book, mid) == 24, "fastmm_hot_book.mid");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_book, bid_px) == 104, "fastmm_hot_book.bid_px");
FASTMM_HOT_STATIC_ASSERT(offsetof(fastmm_hot_book, ask_qty_raw) == 664,
                         "fastmm_hot_book.ask_qty_raw");

#undef FASTMM_HOT_STATIC_ASSERT

#endif /* FASTMM_STRATEGIES_HOT_ABI_H */
