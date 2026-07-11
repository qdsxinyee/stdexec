// test/netexec/test_netexec_tls.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>

#include <catch2/catch_all.hpp>

#include <cstring>

namespace net = netexec::net;

TEST_CASE("tls backend is selected on this platform", "[netexec][tls]") {
#if defined(NETEXEC_TLS_BACKEND_SCHANNEL)
    REQUIRE(true);
#elif defined(NETEXEC_TLS_BACKEND_OPENSSL)
    REQUIRE(true);
#elif defined(NETEXEC_TLS_BACKEND_MBEDTLS)
    REQUIRE(true);
#elif defined(NETEXEC_TLS_BACKEND_SECURE_TRANSPORT)
    REQUIRE(true);
#else
    REQUIRE(false);
#endif
}

TEST_CASE("tls context can be created", "[netexec][tls]") {
    auto ctx = std::make_unique<netexec::net::tls::__detail::context>();
    REQUIRE(ctx != nullptr);

    auto ec = ctx->use_default_trust_store();
    REQUIRE_FALSE(ec);
}

TEST_CASE("tls client session starts a handshake", "[netexec][tls]") {
    auto ctx = std::make_unique<netexec::net::tls::__detail::context>();
    REQUIRE(ctx != nullptr);

    auto session = ctx->create_client_session();
    REQUIRE(session != nullptr);

    std::error_code ec;
    bool            complete = session->handshake_step(ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(complete); // needs network I/O to complete
}

TEST_CASE("tls encrypt requires a completed handshake", "[netexec][tls]") {
    auto ctx = std::make_unique<netexec::net::tls::__detail::context>();
    auto session = ctx->create_client_session();
    REQUIRE(session != nullptr);

    const char      plaintext[] = "hello, tls!";
    char            ciphertext[256]{};
    std::size_t     written = 0;
    std::error_code ec;

    session->encrypt(plaintext, std::strlen(plaintext), ciphertext, sizeof(ciphertext), written, ec);
    REQUIRE(ec);
    REQUIRE(written == 0);
}

TEST_CASE("preconnection defaults to secure", "[netexec][tls]") {
    namespace tls = netexec::net::tls;
    namespace ex = stdexec;
    auto remote = ex::env{net::hostname("localhost"), net::port(12345)};
    tls::preconnection pre(remote);
    REQUIRE(pre.secure());
    REQUIRE(pre.use_system_trust_store());
    REQUIRE(pre.certificate_file().empty());
    REQUIRE(pre.private_key_file().empty());
    REQUIRE(pre.ca_bundle_file().empty());
}

TEST_CASE("preconnection can disable security", "[netexec][tls]") {
    namespace tls = netexec::net::tls;
    namespace ex = stdexec;
    auto remote = ex::env{net::hostname("localhost"), net::port(12345), tls::secure(false)};
    tls::preconnection pre(remote);
    REQUIRE_FALSE(pre.secure());
}

TEST_CASE("preconnection can configure client certificates and CA bundle", "[netexec][tls]") {
    namespace tls = netexec::net::tls;
    namespace ex = stdexec;
    auto remote = ex::env{
        net::hostname("internal.example.com"),
        net::port(443),
        tls::ca_bundle("/path/to/ca.pem"),
        tls::use_system_trust_store(false)};
    tls::preconnection pre(remote);

    REQUIRE(pre.secure());
    REQUIRE(pre.ca_bundle_file() == "/path/to/ca.pem");
    REQUIRE_FALSE(pre.use_system_trust_store());
}

TEST_CASE("preconnection can configure server certificate and key", "[netexec][tls]") {
    namespace tls = netexec::net::tls;
    namespace ex = stdexec;
    auto server = ex::env{
        net::port(443),
        tls::certificate("/etc/ssl/server.crt"),
        tls::private_key("/etc/ssl/server.key")};
    tls::preconnection pre(server);

    REQUIRE(pre.certificate_file() == "/etc/ssl/server.crt");
    REQUIRE(pre.private_key_file() == "/etc/ssl/server.key");
}
