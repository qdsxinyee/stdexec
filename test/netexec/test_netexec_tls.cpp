// test/netexec/test_netexec_tls.cpp
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "test_helpers.hpp"

#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>

#include <catch2/catch_all.hpp>

#include <cstring>
#include <system_error>

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

namespace {

// Server side of the upgrade test: accept through the tls layer, echo one
// message, then hand the TCP connection back to the socket layer and echo
// one more buffer there.
auto upgrade_echo_server(netexec::io_context& ctx, std::uint16_t port, bool* served)
    -> exec::task<void> {
    namespace tls = net::tls;
    tls::preconnection pre(stdexec::env{net::port(port), tls::secure(false)});
    auto acc    = co_await tls::async_listen(pre, ctx);
    auto stream = co_await tls::async_accept(acc);

    char buf[64];
    auto n = co_await tls::async_receive_some(stream, net::buffer(buf));
    co_await tls::async_send(stream, net::message(std::string(buf, n)));

    auto sock = stream.release_socket();
    auto m    = co_await net::ip::tcp::async_receive(sock, net::buffer(buf));
    co_await net::ip::tcp::async_send(sock, net::buffer(buf, m));
    *served = true;
}

// Client side: establish the TCP connection manually, upgrade the connected
// socket into a tls stream, exchange one message, then release the socket
// and continue in the socket layer.
auto upgrade_echo_client(
    netexec::io_context& ctx, std::uint16_t port, bool* echoed_tls, bool* released_plain)
    -> exec::task<void> {
    namespace tls = net::tls;
    net::ip::tcp::socket sock(ctx, netexec_test::make_server_endpoint(port));
    co_await net::ip::tcp::async_connect(sock);

    tls::preconnection pre(stdexec::env{
        net::hostname(std::string("localhost")), net::port(port), tls::secure(false)});
    auto stream = co_await tls::async_initiate(pre, std::move(sock));

    co_await tls::async_send(stream, net::message(std::string("hello")));
    char buf[64];
    auto n      = co_await tls::async_receive_some(stream, net::buffer(buf));
    *echoed_tls = (n == 5 && std::memcmp(buf, "hello", 5) == 0);

    co_await tls::async_shutdown(stream); // no-op for a plaintext stream
    auto raw = stream.release_socket();
    if (stream.secure()) {
        co_return; // release must discard the session
    }
    co_await net::ip::tcp::async_send(raw, net::buffer("world", 5));
    auto m          = co_await net::ip::tcp::async_receive(raw, net::buffer(buf));
    *released_plain = (m == 5 && std::memcmp(buf, "world", 5) == 0);
}

} // namespace

TEST_CASE("netexec - async_initiate upgrades a connected socket", "[netexec][tls]") {
    netexec::scope scope;
    auto           port = netexec_test::next_port();
    bool           served        = false;
    bool           echoed_tls    = false;
    bool           released_plain = false;

    ex::spawn(
        upgrade_echo_server(scope.get_context(), port, &served)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());
    ex::spawn(
        upgrade_echo_client(scope.get_context(), port, &echoed_tls, &released_plain)
            | ex::upon_error([](auto&&) noexcept {}),
        scope.get_token());

    ex::sync_wait(scope.run());
    CHECK(served);
    CHECK(echoed_tls);
    CHECK(released_plain);
}

TEST_CASE("tls session remains valid after its context is destroyed", "[netexec][tls]") {
    std::unique_ptr<netexec::net::tls::__detail::session_base> session;
    {
        auto ctx = std::make_unique<netexec::net::tls::__detail::context>();
        session  = ctx->create_client_session();
        REQUIRE(session != nullptr);
    }
    // The context is gone; the credentials must still be usable because the
    // session shares their ownership. handshake_step drives
    // InitializeSecurityContext with them.
    std::error_code ec;
    bool            complete = session->handshake_step(ec);
    REQUIRE_FALSE(ec);
    REQUIRE_FALSE(complete); // needs network I/O to complete
}

TEST_CASE("make_context reports certificate loading failures", "[netexec][tls]") {
    namespace tls = netexec::net::tls;
    namespace ex  = stdexec;
    auto server = ex::env{net::port(443), tls::certificate("/nonexistent/server.crt")};
    tls::preconnection pre(server);

    try {
        (void)pre.make_context();
        FAIL("expected std::system_error for a missing certificate file");
    } catch (const std::system_error& e) {
        CHECK(std::string(e.what()).find("/nonexistent/server.crt") != std::string::npos);
    }
}
