#pragma once
// Binance internal transfers between the accounts of one master account (Venue::transfer), for
// Binance Spot and USDⓈ-M pools. Control path: blocking REST, allocates.
//
// Endpoint: the master account's universal transfer (sub-account/asset-management
// "Universal Transfer (For Master Account)", USER_DATA, IP weight 1, UID weight 360):
//   POST /sapi/v1/sub-account/universalTransfer  fromEmail, toEmail (absent: the master account),
//        fromAccountType, toAccountType (SPOT | USDT_FUTURE | COIN_FUTURE | MARGIN |
//        ISOLATED_MARGIN), asset, amount, clientTranId (unique), recvWindow, timestamp
//        -> {"tranId":11945860693,"clientTranId":"..."}
//   GET  /sapi/v1/sub-account/universalTransfer  fromEmail or toEmail (not both; absent: the
//        master), clientTranId, startTime (the window is at most 7 days), timestamp
//        -> {"result":[{"tranId":..,"clientTranId":..,"status":"SUCCESS",..}],"totalCount":1}
// It needs a master-account API key with "internal transfer" enabled (spot trading or withdrawals
// are not needed), which is why it is a key of its own (transfer_api_key_env /
// transfer_api_secret_env) and never the account's trading key. The sub-account keys' own
// transfer endpoints (sub-account/transfer/subToSub, subToMaster) move spot assets only and take no
// client id, so a lost answer could not be looked up.
//
// The scenarios the endpoint lists are SPOT, USDT_FUTURE or COIN_FUTURE to SPOT and SPOT to SPOT,
// USDT_FUTURE or COIN_FUTURE, between any two accounts of the master. A spot pool's transfer is one
// leg (SPOT -> SPOT); a USDⓈ-M pool's is two: USDT_FUTURE of the sender -> SPOT of the receiver
// (clientTranId <id>a), then SPOT -> USDT_FUTURE of the receiver (<id>b). Its state is the first
// leg's until that is done, then the second's; a second leg that was not sent (its request failed)
// is sent from transfer_status(), and one the venue refused leaves the amount in the receiver's
// spot wallet (Failed, said so in the detail).
#include "fastmm/core/transfer.hpp"
#include "fastmm/venues/binance/binance_auth.hpp"
#include "fastmm/venues/blocking_control.hpp"
#include "fastmm/venues/blocking_http.hpp"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fastmm::venues::binance {

inline constexpr std::string_view kTransferPath = "/sapi/v1/sub-account/universalTransfer";
inline constexpr std::string_view kDefaultTransferRestUrl = "https://api.binance.com";

struct TransferSettings {
  std::string venue;  // the [venues.<name>] the requests are logged under
  std::string rest_url = std::string(kDefaultTransferRestUrl);
  Credentials credentials;            // the master account's key; HMAC
  std::string account_type = "SPOT";  // the pool's wallet: SPOT | USDT_FUTURE
  std::string email;                  // this account's sub-account email; empty: the master
  int recv_window_ms = 10'000;
  std::string ca_file;
  bool insecure_tls = false;
  std::uint32_t http_timeout_ms = 5000;

  [[nodiscard]] bool usable() const noexcept { return credentials.usable(); }
};

// The keys a Binance connector owns for transfers, read from its [venues.<name>] section:
//   transfer_api_key_env, transfer_api_secret_env  environment variables holding the master key
//   transfer_rest_url                              the SAPI host (default api.binance.com)
//   sub_account_email                              this account's email (empty: the master)
// `account_type` is the connector's wallet. Throws std::invalid_argument when a variable is named
// and not set (outside a dry run), or only one of the two is named.
[[nodiscard]] TransferSettings read_transfer_settings(
    const std::string& venue,
    const std::map<std::string, std::string>& extra,
    std::string_view account_type,
    bool dry_run);

struct TransferLeg {
  std::string client_id;
  std::string from_email;  // empty: the master account
  std::string to_email;
  std::string from_type;
  std::string to_type;
};

// The legs of `req` in a pool whose wallet is `account_type`.
[[nodiscard]] std::vector<TransferLeg> transfer_legs(const TransferRequest& req,
                                                     std::string_view account_type);

// The signed POST of one leg; false when it cannot be built (no usable key, too long).
bool encode_transfer(const Signer& signer,
                     const TransferLeg& leg,
                     std::string_view asset,
                     Notional amount,
                     std::int64_t timestamp_ms,
                     int recv_window_ms,
                     BlockingRequest& out);
// The signed GET that looks a leg up by its client id, from `since_ms` on.
bool encode_transfer_query(const Signer& signer,
                           const TransferLeg& leg,
                           std::int64_t since_ms,
                           std::int64_t timestamp_ms,
                           int recv_window_ms,
                           BlockingRequest& out);

// The answer to the POST: Pending with the tranId on 2xx, Failed on a 4xx (refused, nothing moved),
// Unknown on a transport error or a 5xx.
[[nodiscard]] TransferResult decode_transfer_reply(const HttpReply& reply);
// The answer to the GET for `client_id`: the row's status (SUCCESS: Done; FAILURE or FAILED:
// Failed; anything else: Pending), NotFound without a row, Unknown when the query failed.
[[nodiscard]] TransferResult decode_transfer_history(const HttpReply& reply,
                                                     std::string_view client_id);

// The requests of one connector. `send` defaults to a BlockingControl on settings.rest_url (one per
// request, retried while rate-limited); tests pass their own.
class TransferClient {
 public:
  using Send = std::function<HttpReply(std::string_view label, const BlockingControl::Build&)>;
  using Clock = std::int64_t (*)() noexcept;  // wall ms

  explicit TransferClient(TransferSettings s, Send send = {}, Clock clock = nullptr);

  [[nodiscard]] const TransferSettings& settings() const noexcept { return s_; }
  [[nodiscard]] bool usable() const noexcept { return signer_.usable(); }

  [[nodiscard]] TransferResult transfer(const TransferRequest& req);
  [[nodiscard]] TransferResult status(const TransferRequest& req);

 private:
  [[nodiscard]] TransferResult submit(const TransferRequest& req, const TransferLeg& leg);
  [[nodiscard]] TransferResult query(const TransferRequest& req, const TransferLeg& leg);
  [[nodiscard]] HttpReply send(std::string_view label, const BlockingControl::Build& build);

  TransferSettings s_;
  Signer signer_;
  Send send_;
  Clock clock_;
};

}  // namespace fastmm::venues::binance
