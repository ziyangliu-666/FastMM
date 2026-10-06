#include "fastmm/venues/binance/binance_transfer.hpp"

#include "fastmm/core/time.hpp"
#include "fastmm/venues/binance/binance_params.hpp"
#include "fastmm/venues/binance/binance_rest_decoder.hpp"

#include <simdjson.h>

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace fastmm::venues::binance {

namespace sj = simdjson;
namespace dom = simdjson::dom;

TransferSettings read_transfer_settings(const std::string& venue,
                                        const std::map<std::string, std::string>& extra,
                                        std::string_view account_type,
                                        bool dry_run) {
  const auto get = [&](const char* k) {
    const auto it = extra.find(k);
    return it == extra.end() ? std::string{} : it->second;
  };
  TransferSettings s;
  s.venue = venue;
  s.account_type = std::string(account_type);
  s.email = get("sub_account_email");
  if (std::string url = get("transfer_rest_url"); !url.empty()) s.rest_url = std::move(url);
  const std::string key_env = get("transfer_api_key_env");
  const std::string secret_env = get("transfer_api_secret_env");
  if (key_env.empty() != secret_env.empty()) {
    throw std::invalid_argument("venue '" + venue +
                                "': transfer_api_key_env and transfer_api_secret_env go together");
  }
  if (key_env.empty() || dry_run) return s;
  const char* key = std::getenv(key_env.c_str());
  const char* secret = std::getenv(secret_env.c_str());
  if (key == nullptr || *key == '\0' || secret == nullptr || *secret == '\0') {
    throw std::invalid_argument("venue '" + venue + "': environment variable '" +
                                (key == nullptr || *key == '\0' ? key_env : secret_env) +
                                "' (the transfer key) is not set");
  }
  s.credentials.api_key = key;
  s.credentials.secret.value = secret;
  s.credentials.type = KeyType::Hmac;
  return s;
}

std::vector<TransferLeg> transfer_legs(const TransferRequest& req, std::string_view account_type) {
  std::vector<TransferLeg> legs;
  if (account_type == "SPOT") {
    legs.push_back({req.client_id, req.from_account, req.to_account, "SPOT", "SPOT"});
    return legs;
  }
  // A futures wallet goes through the receiver's spot wallet (the endpoint has no futures to
  // futures scenario).
  const std::string type(account_type);
  legs.push_back({req.client_id + "a", req.from_account, req.to_account, type, "SPOT"});
  legs.push_back({req.client_id + "b", req.to_account, req.to_account, "SPOT", type});
  return legs;
}

namespace {

// Parameters in alphabetical order (BinanceParams checks it): amount < asset < clientTranId <
// fromAccountType < fromEmail < recvWindow < startTime < timestamp < toAccountType < toEmail.
using TransferParams = BinanceParams<10>;

bool finish(const Signer& signer,
            const TransferParams& p,
            std::string_view method,
            BlockingRequest& out) {
  net::QueryBuilder<kMaxRequestBytes> q;
  if (!signer.usable() || !write_signed_rest_query(p, signer, q)) return false;
  out.method = std::string(method);
  out.target = std::string(kTransferPath) + "?" + std::string(q.view());
  out.headers = api_key_header(signer.api_key());
  out.body.clear();
  return true;
}

}  // namespace

bool encode_transfer(const Signer& signer,
                     const TransferLeg& leg,
                     std::string_view asset,
                     Notional amount,
                     std::int64_t timestamp_ms,
                     int recv_window_ms,
                     BlockingRequest& out) {
  if (amount.raw <= 0 || asset.empty() || leg.client_id.empty()) return false;
  TransferParams p;
  p.add_decimal("amount", amount);
  p.add("asset", asset);
  p.add("clientTranId", leg.client_id);
  p.add("fromAccountType", leg.from_type);
  if (!leg.from_email.empty()) p.add("fromEmail", leg.from_email);
  p.add_int("recvWindow", recv_window_ms);
  p.add_int("timestamp", timestamp_ms);
  p.add("toAccountType", leg.to_type);
  if (!leg.to_email.empty()) p.add("toEmail", leg.to_email);
  return finish(signer, p, "POST", out);
}

bool encode_transfer_query(const Signer& signer,
                           const TransferLeg& leg,
                           std::int64_t since_ms,
                           std::int64_t timestamp_ms,
                           int recv_window_ms,
                           BlockingRequest& out) {
  if (leg.client_id.empty()) return false;
  TransferParams p;
  p.add("clientTranId", leg.client_id);
  // One of the two emails at most; the history defaults to transfers from the master account.
  if (!leg.from_email.empty()) p.add("fromEmail", leg.from_email);
  p.add_int("recvWindow", recv_window_ms);
  if (since_ms > 0) p.add_int("startTime", since_ms);
  p.add_int("timestamp", timestamp_ms);
  return finish(signer, p, "GET", out);
}

namespace {

std::string error_text(const HttpReply& reply) {
  if (!reply.error.empty()) return reply.error;
  int code = 0;
  std::string msg;
  if (decode_rest_error(reply.body, code, msg))
    return "HTTP " + std::to_string(reply.status) + " code " + std::to_string(code) + " " + msg;
  return "HTTP " + std::to_string(reply.status) + " " + reply.body.substr(0, 120);
}

std::string id_text(const dom::element& e) {
  std::int64_t n = 0;
  if (e.get(n) == sj::SUCCESS) return std::to_string(n);
  std::uint64_t u = 0;
  if (e.get(u) == sj::SUCCESS) return std::to_string(u);
  std::string_view s;
  if (e.get(s) == sj::SUCCESS) return std::string(s);
  return {};
}

}  // namespace

TransferResult decode_transfer_reply(const HttpReply& reply) {
  TransferResult r;
  if (reply.status == 0 || reply.status >= 500) {
    r.state = TransferState::Unknown;
    r.detail = error_text(reply);
    return r;
  }
  if (!reply.ok()) {
    r.state = TransferState::Failed;
    r.detail = error_text(reply);
    return r;
  }
  dom::parser parser;
  dom::element root;
  dom::element id;
  if (parser.parse(sj::padded_string(reply.body)).get(root) != sj::SUCCESS ||
      root["tranId"].get(id) != sj::SUCCESS) {
    // A 2xx without a transfer id: the venue may have taken it; its history says.
    r.state = TransferState::Unknown;
    r.detail = "no tranId in " + reply.body.substr(0, 120);
    return r;
  }
  r.state = TransferState::Pending;
  r.venue_ref = id_text(id);
  return r;
}

TransferResult decode_transfer_history(const HttpReply& reply, std::string_view client_id) {
  TransferResult r;
  if (!reply.ok()) {
    r.state = TransferState::Unknown;
    r.detail = error_text(reply);
    return r;
  }
  dom::parser parser;
  dom::element root;
  if (parser.parse(sj::padded_string(reply.body)).get(root) != sj::SUCCESS) {
    r.state = TransferState::Unknown;
    r.detail = "transfer history: invalid JSON";
    return r;
  }
  dom::array rows;
  if (root["result"].get(rows) != sj::SUCCESS) {
    r.state = TransferState::NotFound;  // {"totalCount":0} carries no result[]
    return r;
  }
  for (dom::element row : rows) {
    std::string_view id;
    if (row["clientTranId"].get(id) != sj::SUCCESS || id != client_id) continue;
    std::string_view status;
    if (row["status"].get(status) != sj::SUCCESS) status = {};
    dom::element tran;
    if (row["tranId"].get(tran) == sj::SUCCESS) r.venue_ref = id_text(tran);
    if (status == "SUCCESS") {
      r.state = TransferState::Done;
    } else if (status == "FAILURE" || status == "FAILED") {
      r.state = TransferState::Failed;
      r.detail = "the venue reports " + std::string(status);
    } else {
      r.state = TransferState::Pending;
      r.detail = std::string(status);
    }
    return r;
  }
  r.state = TransferState::NotFound;
  return r;
}

// ---- client -------------------------------------------------------------------------------------

namespace {
std::int64_t wall_ms() noexcept {
  return wall_now().ns / 1'000'000;
}
// Looked up from a minute before the transfer was created: the two clocks differ a little.
constexpr std::int64_t kQuerySlackMs = 60'000;
}  // namespace

TransferClient::TransferClient(TransferSettings s, Send send, Clock clock)
    : s_(std::move(s)),
      signer_(s_.credentials),
      send_(std::move(send)),
      clock_(clock != nullptr ? clock : &wall_ms) {}

HttpReply TransferClient::send(std::string_view label, const BlockingControl::Build& build) {
  if (send_) return send_(label, build);
  BlockingHttpOptions o;
  o.ca_file = s_.ca_file;
  o.insecure_tls = s_.insecure_tls;
  o.timeout_ms = s_.http_timeout_ms;
  BlockingRetry retry;
  retry.max_retries = 2;
  retry.deadline_ms = 8'000;
  BlockingControl control(s_.venue, s_.rest_url, o, retry);
  return control.send(label, build);
}

TransferResult TransferClient::submit(const TransferRequest& req, const TransferLeg& leg) {
  BlockingRequest probe;
  if (!encode_transfer(signer_, leg, req.asset, req.amount, clock_(), s_.recv_window_ms, probe))
    return {TransferState::Failed, {}, "the transfer request cannot be built"};
  const HttpReply reply = send("transfer " + leg.client_id, [&](BlockingRequest& q) {
    return encode_transfer(signer_, leg, req.asset, req.amount, clock_(), s_.recv_window_ms, q);
  });
  return decode_transfer_reply(reply);
}

TransferResult TransferClient::query(const TransferRequest& req, const TransferLeg& leg) {
  const std::int64_t since = req.created_ms > kQuerySlackMs ? req.created_ms - kQuerySlackMs : 0;
  const HttpReply reply = send("transfer status " + leg.client_id, [&](BlockingRequest& q) {
    return encode_transfer_query(signer_, leg, since, clock_(), s_.recv_window_ms, q);
  });
  return decode_transfer_history(reply, leg.client_id);
}

TransferResult TransferClient::transfer(const TransferRequest& req) {
  if (!usable()) return {TransferState::Failed, {}, "no transfer key (transfer_api_key_env)"};
  const std::vector<TransferLeg> legs = transfer_legs(req, s_.account_type);
  TransferResult first = submit(req, legs[0]);
  if (legs.size() == 1 || first.state != TransferState::Pending) return first;
  // The second leg moves what the first put in the receiver's spot wallet. A refusal here (the
  // first leg not yet credited) is retried by status().
  const TransferResult second = submit(req, legs[1]);
  if (second.state == TransferState::Failed) {
    first.detail = "second step not taken yet: " + second.detail;
  }
  return first;
}

TransferResult TransferClient::status(const TransferRequest& req) {
  if (!usable()) return {TransferState::Unknown, {}, "no transfer key (transfer_api_key_env)"};
  const std::vector<TransferLeg> legs = transfer_legs(req, s_.account_type);
  TransferResult r = query(req, legs[0]);
  if (legs.size() == 1 || r.state != TransferState::Done) return r;
  TransferResult second = query(req, legs[1]);
  if (second.state == TransferState::NotFound) {
    second = submit(req, legs[1]);
    if (second.state == TransferState::Failed) {
      second.detail = "the amount is in the receiving account's SPOT wallet: " + second.detail;
      return second;
    }
    return {TransferState::Pending, second.venue_ref, "second step sent"};
  }
  if (second.state == TransferState::Failed) {
    second.detail = "the amount is in the receiving account's SPOT wallet: " + second.detail;
  }
  return second;
}

}  // namespace fastmm::venues::binance
