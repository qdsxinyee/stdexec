// test/netexec/test_netexec_endpoint.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

TEST_CASE("netexec - address_v4 constants", "[netexec][endpoint]") {
    auto any       = net::ip::address_v4::any();
    auto loop      = net::ip::address_v4::loopback();
    auto broadcast = net::ip::address_v4::broadcast();

    CHECK(any.is_unspecified());
    CHECK(loop.is_loopback());
    CHECK(!any.is_loopback());
    CHECK(!loop.is_unspecified());
}

TEST_CASE("netexec - tcp endpoint construction", "[netexec][endpoint]") {
    net::ip::tcp::endpoint ep(net::ip::address_v4::loopback(), 12345);

    CHECK(ep.address().to_v4().to_uint() == net::ip::address_v4::loopback().to_uint());
    CHECK(ep.port() == 12345);
}

TEST_CASE("netexec - tcp endpoint with any address", "[netexec][endpoint]") {
    net::ip::tcp::endpoint ep(net::ip::address_v4::any(), 0);

    CHECK(ep.address().to_v4().to_uint() == net::ip::address_v4::any().to_uint());
    CHECK(ep.port() == 0);
}

TEST_CASE("netexec - make_address parses IPv4", "[netexec][endpoint]") {
    auto addr = net::ip::make_address("127.0.0.1");

    CHECK(addr.is_v4());
    CHECK(addr.to_v4().to_uint() == net::ip::address_v4::loopback().to_uint());
}

TEST_CASE("netexec - make_address parses IPv6", "[netexec][endpoint]") {
    auto addr = net::ip::make_address("::1");

    CHECK(addr.is_v6());
    unsigned char loopback[16] = {};
    loopback[15] = 1;
    CHECK(addr.to_v6() == net::ip::address_v6(loopback));
}

TEST_CASE("netexec - make_address rejects invalid input", "[netexec][endpoint]") {
    CHECK_THROWS(net::ip::make_address("not-an-address"));
    CHECK_THROWS(net::ip::make_address(""));
}
