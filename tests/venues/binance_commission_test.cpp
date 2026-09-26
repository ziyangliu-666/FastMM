// GET /api/v3/account/commission (rest-api.md "Query Commission Rates"): the signed request and
// the rates a fill pays, standard + special + tax commission (commission_faq.md).
#include "test_support.hpp"

#include "fastmm/net/crypto.hpp"
#include "fastmm/venues/binance/binance_order_encoder.hpp"
#include "fastmm/venues/binance/binance_rest_decoder.hpp"

#include <string>

using namespace fastmm;
using namespace fastmm::venues;
using namespace fastmm::venues::binance;

TEST_CASE("binance.commission: standard, special and tax commission add up per liquidity") {
  // The example of rest-api.md: maker 0.0000001 + 0.01 + 0.00000112, the larger of buyer and
  // seller in each group added to both sides.
  const char* body = R"({"symbol":"BTCUSDT",
    "standardCommission":{"maker":"0.00000010","taker":"0.00000020","buyer":"0.00000030","seller":"0.00000040"},
    "specialCommission":{"maker":"0.01000000","taker":"0.02000000","buyer":"0.03000000","seller":"0.04000000"},
    "taxCommission":{"maker":"0.00000112","taker":"0.00000114","buyer":"0.00000118","seller":"0.00000116"},
    "discount":{"enabledForAccount":true,"enabledForSymbol":true,"discountAsset":"BNB","discount":"0.75000000"}})";
  CommissionRates c;
  REQUIRE(decode_commission(body, c).empty());
  CHECK(c.symbol == "BTCUSDT");
  CHECK(c.side_dependent);
  // maker: 0.1 + 10000 + 1.12 + (0.4 + 40000 + 1.18) cbps = 50002.8 -> 50003
  CHECK(c.rates.maker_cbps == 50003);
  // taker: 0.2 + 20000 + 1.14 + 40001.58 = 60002.92 -> 60003
  CHECK(c.rates.taker_cbps == 60003);

  // A VIP 0 spot account: 0.1 % both ways, nothing else.
  const char* vip0 = R"({"symbol":"ETHUSDT",
    "standardCommission":{"maker":"0.00100000","taker":"0.00100000","buyer":"0.00000000","seller":"0.00000000"},
    "specialCommission":{"maker":"0.00000000","taker":"0.00000000","buyer":"0.00000000","seller":"0.00000000"},
    "taxCommission":{"maker":"0.00000000","taker":"0.00000000","buyer":"0.00000000","seller":"0.00000000"}})";
  REQUIRE(decode_commission(vip0, c).empty());
  CHECK_FALSE(c.side_dependent);
  CHECK(c.rates == FeeRates::from_bps(10.0, 10.0));
  // A zero-maker pair.
  const char* zero = R"({"symbol":"BTCU",
    "standardCommission":{"maker":"0.00000000","taker":"0.00100000","buyer":"0.00000000","seller":"0.00000000"}})";
  REQUIRE(decode_commission(zero, c).empty());
  CHECK(c.rates.maker_cbps == 0);
  CHECK(c.rates.taker_cbps == 1000);

  CHECK_FALSE(decode_commission(R"({"symbol":"X"})", c).empty());
  CHECK_FALSE(decode_commission(R"({"symbol":"X","standardCommission":{"maker":"x"}})", c).empty());
  CHECK_FALSE(decode_commission("[]", c).empty());
}

TEST_CASE("binance.commission: the request is signed and names the symbol") {
  Credentials cr;
  cr.api_key = "k";
  cr.secret.value = "s";
  Signer signer(cr);
  SymbolTable symbols;
  BinanceOrderEncoder enc(signer, symbols, 5000);
  RestRequest rr;
  REQUIRE(enc.encode_rest_commission("BTCUSDT", 1789295134000, rr));
  CHECK(rr.method == "GET");
  CHECK(rr.path == "/api/v3/account/commission");
  CHECK(rr.weight == 20);
  const std::string q(rr.query.view());
  const std::size_t sig = q.rfind("&signature=");
  REQUIRE(sig != std::string::npos);
  CHECK(q.substr(0, sig) == "symbol=BTCUSDT&timestamp=1789295134000");
  CHECK(q.substr(sig + 11) == std::string(net::hmac_sha256_hex("s", q.substr(0, sig)).view()));
  CHECK_FALSE(enc.encode_rest_commission("", 1, rr));
}
