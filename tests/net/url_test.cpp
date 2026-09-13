#include "fastmm/net/url.hpp"

#include "test_support.hpp"

#include <string>

using namespace fastmm::net;

TEST_CASE("url: parse full wss URL") {
  auto u = Url::parse("wss://stream.binance.com:9443/stream?streams=btcusdt@depth");
  REQUIRE(u.has_value());
  CHECK(u->scheme == "wss");
  CHECK(u->host == "stream.binance.com");
  CHECK(u->port == 9443);
  CHECK(u->path == "/stream");
  CHECK(u->query == "streams=btcusdt@depth");
  CHECK(u->tls);
  char buf[128];
  const auto n = u->request_target(buf);
  CHECK(std::string_view(buf, n) == "/stream?streams=btcusdt@depth");
}

TEST_CASE("url: scheme defaults and edge cases") {
  auto a = Url::parse("https://api.example.com");
  REQUIRE(a);
  CHECK(a->port == 443);
  CHECK(a->path == "/");
  CHECK(a->query.empty());
  CHECK(a->tls);

  auto b = Url::parse("ws://localhost:8080");
  REQUIRE(b);
  CHECK(b->port == 8080);
  CHECK_FALSE(b->tls);
  CHECK(b->path == "/");

  auto c = Url::parse("http://h?x=1");
  REQUIRE(c);
  CHECK(c->port == 80);
  CHECK(c->path == "/");
  CHECK(c->query == "x=1");

  auto d = Url::parse("wss://[::1]:9443/ws");
  REQUIRE(d);
  CHECK(d->host == "::1");
  CHECK(d->port == 9443);
  CHECK(d->path == "/ws");

  auto e = Url::parse("ws://127.0.0.1:1/a/b/c?q");
  REQUIRE(e);
  CHECK(e->host == "127.0.0.1");
  CHECK(e->path == "/a/b/c");
  CHECK(e->query == "q");
}

TEST_CASE("url: rejects malformed input") {
  CHECK_FALSE(Url::parse("no-scheme.com/path"));
  CHECK_FALSE(Url::parse("://host"));
  CHECK_FALSE(Url::parse("wss://"));
  CHECK_FALSE(Url::parse("wss://:443"));
  CHECK_FALSE(Url::parse("wss://host:abc"));
  CHECK_FALSE(Url::parse("wss://host:70000"));
  CHECK_FALSE(Url::parse("wss://host:0"));
  CHECK_FALSE(Url::parse("wss://[::1/x"));
}

TEST_CASE("url: percent_encode") {
  char out[64];
  auto n = percent_encode("a b&c=d/é~", out);
  CHECK(std::string_view(out, n) == "a%20b%26c%3Dd%2F%C3%A9~");
  CHECK(percent_encode("abc", std::span<char>(out, 2)) == 0);
  n = percent_encode("", out);
  CHECK(n == 0);
}

TEST_CASE("url: QueryBuilder") {
  QueryBuilder<128> q;
  q.add("symbol", "BTCUSDT")
      .add("side", "BUY")
      .add("quantity", "0.001")
      .add("ts", std::int64_t{1700000000123});
  CHECK(q.ok());
  CHECK(q.view() == "symbol=BTCUSDT&side=BUY&quantity=0.001&ts=1700000000123");
  q.add("neg", std::int64_t{-42});
  CHECK(q.view() == "symbol=BTCUSDT&side=BUY&quantity=0.001&ts=1700000000123&neg=-42");
  q.append_raw("&signature=abc");
  CHECK(q.view() ==
        "symbol=BTCUSDT&side=BUY&quantity=0.001&ts=1700000000123&neg=-42&signature=abc");

  SUBCASE("values are percent-encoded") {
    QueryBuilder<64> p;
    p.add("streams", "btcusdt@depth/btcusdt@trade");
    CHECK(p.view() == "streams=btcusdt%40depth%2Fbtcusdt%40trade");
  }
  SUBCASE("overflow is sticky and rolls back the partial pair") {
    QueryBuilder<16> s;
    s.add("a", "1");
    CHECK(s.ok());
    s.add("bbbbbbbbbb", "cccccccc");
    CHECK_FALSE(s.ok());
    CHECK(s.view() == "a=1");
    s.add("d", "2");  // ignored once failed
    CHECK(s.view() == "a=1");
    s.clear();
    CHECK(s.ok());
    CHECK(s.empty());
  }
}
