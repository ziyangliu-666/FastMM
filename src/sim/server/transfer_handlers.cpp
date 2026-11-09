// Internal transfers between the accounts of one master account: POST and GET
// /sapi/v1/sub-account/universalTransfer (SPOT to SPOT), signed with the master's key, with the
// balanceUpdate + outboundAccountPosition pair on both accounts' user streams and the transfer
// faults of [sim.faults].
#include "server_impl.hpp"

#include <algorithm>

namespace fastmm::sim::server {

using Impl = SimExchangeServer::Impl;

namespace {

// UID weight of one POST (the master account's universal transfer).
constexpr std::uint32_t kTransferUidWeight = 360;
constexpr std::int64_t kHistoryWindowMs = 7LL * 86'400'000;
constexpr std::string_view kNoAmount = "user have no available amount";

std::string mandatory(std::string_view name) {
  return "Mandatory parameter '" + std::string(name) +
         "' was not sent, was empty/null, or malformed.";
}

std::string_view status_text(TransferStatus s) noexcept {
  switch (s) {
    case TransferStatus::Process:
      return "PROCESS";
    case TransferStatus::Success:
      return "SUCCESS";
    case TransferStatus::Failure:
      return "FAILURE";
  }
  return "PROCESS";
}

OpResult too_many(const std::string& msg, std::int64_t wait_ms) {
  OpResult r = OpResult::error(429, -1003, msg);
  r.retry_after_s = std::max<std::int64_t>(1, (wait_ms + 999) / 1000);
  return r;
}

}  // namespace

Account* Impl::account_by_email(std::string_view email) noexcept {
  if (email.empty() || email == master_.email) return &master_;
  for (Account& a : accounts_) {
    if (a.email == email) return &a;
  }
  return nullptr;
}

// splitmix64, uniform in [0, 1).
double Impl::next_transfer_draw() noexcept {
  std::uint64_t z = (transfer_rng_ += 0x9E37'79B9'7F4A'7C15ULL);
  z = (z ^ (z >> 30U)) * 0xBF58'476D'1CE4'E5B9ULL;
  z = (z ^ (z >> 27U)) * 0x94D0'49BB'1331'11EBULL;
  z ^= z >> 31U;
  return static_cast<double>(z >> 11U) * 0x1.0p-53;
}

OpResult Impl::op_transfer(const ParamList& p, std::int64_t now_ms) {
  ++stats_.transfer_requests;
  const TransferFaultConfig& f = faults_.transfer;
  if (f.ban_after > 0 && transfers_1m_.add(1, now_ms) > f.ban_after) {
    ++stats_.transfers_rate_limited;
    return too_many("Too many requests; current limit is " + std::to_string(f.ban_after) +
                        " transfers per 1 MINUTE.",
                    transfers_1m_.ms_until_reset(now_ms));
  }
  if (uid_weight_1m_.add(kTransferUidWeight, now_ms) > cfg_.uid_weight_limit_per_minute) {
    ++stats_.transfers_rate_limited;
    return too_many("Too much request weight used; current limit of UID is " +
                        std::to_string(cfg_.uid_weight_limit_per_minute) +
                        " request weight per 1 MINUTE.",
                    uid_weight_1m_.ms_until_reset(now_ms));
  }

  auto refuse = [this](int code, std::string_view msg) {
    ++stats_.transfers_refused;
    return OpResult::error(400, code, msg);
  };
  const std::string_view asset = p.get("asset");
  if (asset.empty()) return refuse(-1102, mandatory("asset"));
  const std::string_view amount_text = p.get("amount");
  if (amount_text.empty()) return refuse(-1102, mandatory("amount"));
  if (!is_decimal_syntax(amount_text))
    return refuse(-1100, "Illegal characters found in parameter 'amount'.");
  const auto amount = Qty::from_decimal(amount_text);
  if (!amount) return refuse(-1111, "Parameter 'amount' has too much precision.");
  if (!amount->is_positive()) return refuse(-1130, "Invalid amount.");
  for (const std::string_view type : {"fromAccountType", "toAccountType"}) {
    if (!p.has(type)) return refuse(-1102, mandatory(type));
    if (p.get(type) != "SPOT")
      return refuse(-1130, "Invalid " + std::string(type) + ": only SPOT to SPOT is simulated.");
  }
  Account* from = account_by_email(p.get("fromEmail"));
  if (from == nullptr) return refuse(-1130, "Invalid fromEmail.");
  Account* to = account_by_email(p.get("toEmail"));
  if (to == nullptr) return refuse(-1130, "Invalid toEmail.");
  if (from == to) return refuse(-1130, "fromEmail and toEmail are the same account.");
  const std::string client_id(p.get("clientTranId"));
  if (!client_id.empty() && transfers_by_client_.contains(client_id))
    return refuse(-1130, "Duplicate clientTranId.");
  if (std::find(f.refused_assets.begin(), f.refused_assets.end(), asset) != f.refused_assets.end())
    return refuse(-9000, kNoAmount);
  if (from->balance(asset).free < *amount) return refuse(-9000, kNoAmount);
  // Both draws for every transfer that gets this far, so one seed gives one sequence.
  const bool fail = next_transfer_draw() < f.p_fail;
  const bool lost = next_transfer_draw() < f.p_lost;
  if (fail) return refuse(-1000, "An unknown error occurred while processing the request.");

  const std::size_t index = transfers_.size();
  TransferRecord t;
  t.tran_id = next_tran_id_++;
  t.client_tran_id = client_id;
  t.from = from;
  t.to = to;
  t.asset = std::string(asset);
  t.amount = *amount;
  t.create_ms = now_ms;
  transfers_.push_back(std::move(t));
  if (!client_id.empty()) transfers_by_client_.emplace(client_id, index);
  if (f.delay_ms == 0) {
    complete_transfer(index);  // its events follow when the request ends
  } else {
    reactor_.add_timer_after(static_cast<std::int64_t>(f.delay_ms) * kNsPerMs, [this, index] {
      begin_request(0);
      complete_transfer(index);
      end_request();
    });
  }

  if (lost) {
    ++stats_.transfers_lost;
    if (faults_.transfers_lost++ % 2 == 0) {
      OpResult r;
      r.no_reply = true;
      return r;
    }
    return OpResult::error(
        503, -1001, "Internal error; unable to process your request. Please try again.");
  }
  std::string body;
  JsonObjectWriter w(body);
  w.num("tranId", transfers_[index].tran_id);
  if (!client_id.empty()) w.str("clientTranId", client_id);
  w.close();
  return OpResult::ok(std::move(body));
}

void Impl::complete_transfer(std::size_t index) {
  TransferRecord& t = transfers_[index];
  Balance& out = t.from->balance(t.asset);
  if (out.free < t.amount) {
    // Taken, then short when its turn came: the venue reports it FAILURE and moves nothing.
    t.status = TransferStatus::Failure;
    ++stats_.transfers_failed_late;
    return;
  }
  out.free -= t.amount;
  t.to->balance(t.asset).free += t.amount;
  t.status = TransferStatus::Success;
  ++stats_.transfers_done;
  const std::int64_t ms = server_ms();
  for (const auto& [acct, delta] : {std::pair{t.from, -t.amount}, std::pair{t.to, t.amount}}) {
    if (acct == &master_) continue;  // the master has no user stream here
    std::string event;
    append_balance_update(event, ms, t.asset, delta, ms);
    emit_user_event(acct->id, std::move(event));
    acct->touched(t.asset);  // outboundAccountPosition after it, when the request ends
  }
}

OpResult Impl::op_transfer_history(const ParamList& p, std::int64_t now_ms) {
  const std::string_view from_email = p.get("fromEmail");
  const std::string_view to_email = p.get("toEmail");
  if (!from_email.empty() && !to_email.empty())
    return OpResult::error(400, -1130, "fromEmail and toEmail cannot be sent at the same time.");
  std::int64_t start = now_ms - kHistoryWindowMs;
  std::int64_t end = now_ms;
  if (p.has("startTime")) {
    const auto v = parse_int(p.get("startTime"));
    if (!v)
      return OpResult::error(400, -1100, "Illegal characters found in parameter 'startTime'.");
    start = *v;
    end = std::max(end, start);
  }
  if (p.has("endTime")) {
    const auto v = parse_int(p.get("endTime"));
    if (!v) return OpResult::error(400, -1100, "Illegal characters found in parameter 'endTime'.");
    end = *v;
  }
  if (end - start > kHistoryWindowMs)
    return OpResult::error(400, -1127, "More than 7 days between startTime and endTime.");
  const std::int64_t page = std::max<std::int64_t>(1, parse_int(p.get("page")).value_or(1));
  const std::int64_t limit =
      std::clamp<std::int64_t>(parse_int(p.get("limit")).value_or(500), 1, 500);

  // fromEmail: transfers out of that account; toEmail: into it; neither: the master's.
  const Account* party = nullptr;
  if (!from_email.empty() || !to_email.empty()) {
    party = account_by_email(from_email.empty() ? to_email : from_email);
  } else {
    party = &master_;
  }
  const std::string_view client_id = p.get("clientTranId");
  std::vector<const TransferRecord*> rows;
  if (party != nullptr) {
    for (auto it = transfers_.rbegin(); it != transfers_.rend(); ++it) {
      const TransferRecord& t = *it;
      if (!from_email.empty() && t.from != party) continue;
      if (!to_email.empty() && t.to != party) continue;
      if (from_email.empty() && to_email.empty() && t.from != party && t.to != party) continue;
      if (!client_id.empty() && t.client_tran_id != client_id) continue;
      if (t.create_ms < start || t.create_ms > end) continue;
      rows.push_back(&t);
    }
  }
  std::string body;
  if (rows.empty()) {
    body = R"({"totalCount":0})";
    return OpResult::ok(std::move(body));
  }
  body = R"({"result":[)";
  const auto first = static_cast<std::size_t>(
      std::min<std::int64_t>((page - 1) * limit, static_cast<std::int64_t>(rows.size())));
  const std::size_t last = std::min(rows.size(), first + static_cast<std::size_t>(limit));
  for (std::size_t i = first; i < last; ++i) {
    const TransferRecord& t = *rows[i];
    if (i != first) body.push_back(',');
    JsonObjectWriter w(body);
    w.num("tranId", t.tran_id)
        .str("fromEmail", t.from->email)
        .str("toEmail", t.to->email)
        .str("asset", t.asset)
        .dec("amount", t.amount)
        .num("createTimeStamp", t.create_ms)
        .str("fromAccountType", "SPOT")
        .str("toAccountType", "SPOT")
        .str("status", status_text(t.status))
        .str("clientTranId", t.client_tran_id);
    w.close();
  }
  body += "],\"totalCount\":" + std::to_string(rows.size()) + "}";
  return OpResult::ok(std::move(body));
}

}  // namespace fastmm::sim::server
