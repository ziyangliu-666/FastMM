#include "test_support.hpp"

#include <openssl/opensslv.h>
TEST_CASE("net.smoke: OpenSSL >= 3") {
  CHECK(OPENSSL_VERSION_MAJOR >= 3);
}
