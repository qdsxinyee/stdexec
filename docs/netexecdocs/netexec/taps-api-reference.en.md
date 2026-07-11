# `netexec::net::tls` API Reference

> Based on final option B: tls layer APIs live in `netexec::net::tls` and use the `async_*` prefix; socket APIs live in `netexec::net::ip::tcp` and use the `async_*` prefix for single-system-call operations.

---

## Namespace Hierarchy

```cpp
netexec::net::tls::*            // Optional tls layer (declarative, message-oriented, TLS opt-in)
netexec::net::ip::tcp::*        // Socket API (raw, partial-byte, manual control)
```

`netexec::net::ip::tcp::*` provides plain TCP socket operations. The tls layer at `netexec::net::tls::*` is opt-in.

---

## tls layer API

```cpp
namespace netexec::net::tls {

// Actively establish a connection based on a preconnection.
// Internal dependencies:
//   - netexec::net::ip::tcp::async_resolve   (when preconnection uses a hostname)
//   - netexec::net::ip::tcp::async_connect   (completes the TCP three-way handshake)
//   - TLS handshake                          (when the preconnection is secure)
inline constexpr struct async_initiate_t { ... } async_initiate{};
// Signature: async_initiate(const preconnection&, io_context&) -> sender<stream>

// Create a listening endpoint based on a preconnection.
// Internal dependencies:
//   - Constructs a netexec::net::ip::tcp::acceptor
//   - acceptor.bind(endpoint)
//   - acceptor.listen()
// Typically used afterwards with:
//   - netexec::net::tls::async_accept    (to accept a tls::stream)
inline constexpr struct async_listen_t { ... } async_listen{};
// Signature: async_listen(const preconnection&, io_context&) -> sender<acceptor>

// Accept a new TLS connection.
// Internal dependencies:
//   - netexec::net::ip::tcp::async_accept   (accepts a raw tcp::socket)
//   - TLS handshake                         (when the acceptor is secure)
inline constexpr struct async_accept_t { ... } async_accept{};
// Signature: async_accept(acceptor&) -> sender<stream>

// Send a complete message.
// Internal dependencies:
//   - May loop over netexec::net::ip::tcp::async_send
//   - May pass through a framer for encoding
//   - Passes through TLS encryption/wrapping
inline constexpr struct async_send_t { ... } async_send{};
// Signature: async_send(stream&, const message&) -> sender<void>

// Receive a complete message.
// Internal dependencies:
//   - May loop over netexec::net::ip::tcp::async_receive
//   - May pass through a framer for decoding
//   - Passes through TLS decryption/unwrapping
inline constexpr struct async_receive_t { ... } async_receive{};
// Signature: async_receive(stream&) -> sender<message>

// Gracefully shut down a TLS connection by sending close_notify.
// Plain TCP streams are no-ops.
inline constexpr struct async_shutdown_t { ... } async_shutdown{};
// Signature: async_shutdown(stream&) -> sender<void>

} // namespace netexec::net::tls
```

---

## Connection Types

```cpp
namespace netexec::net::tls {

class stream { /* ... */ };    // A TLS-wrapped connection
class acceptor { /* ... */ };  // A TLS listening endpoint

} // namespace netexec::net::tls
```

---

## Socket API

```cpp
namespace netexec::net::ip::tcp {

// Resolve a hostname to endpoints.
// Signature: async_resolve(io_context&, const std::string& hostname, std::uint16_t port)
//            -> sender<std::vector<endpoint>>
inline constexpr struct async_resolve_t { ... } async_resolve{};

// Initiate a TCP connection (one system-call-level asynchronous connect)
inline constexpr struct async_connect_t { ... } async_connect{};
// Signature: async_connect(socket&, const endpoint&) -> sender<void>

// Bind and listen on an acceptor (explicit bind + listen)
inline constexpr struct async_listen_t { ... } async_listen{};
// Signature: async_listen(acceptor&, const endpoint&) -> sender<void>

// Accept a TCP connection (one system-call-level asynchronous accept)
inline constexpr struct async_accept_t { ... } async_accept{};
// Signature: async_accept(acceptor&) -> sender<socket, endpoint>

// Send some bytes (may be partial)
inline constexpr struct async_send_t { ... } async_send{};
// Signature: async_send(socket&, const_buffer) -> sender<size_t>

// Receive some bytes (may be partial)
inline constexpr struct async_receive_t { ... } async_receive{};
// Signature: async_receive(socket&, mutable_buffer) -> sender<size_t>

} // namespace netexec::net::ip::tcp
```

---

## Dependency Graph

```
netexec::net::tls::async_initiate
    ├── netexec::net::ip::tcp::async_resolve
    ├── netexec::net::ip::tcp::async_connect
    └── TLS handshake

netexec::net::tls::async_listen
    ├── netexec::net::ip::tcp::acceptor::bind
    └── netexec::net::ip::tcp::acceptor::listen
    └── (used with) netexec::net::tls::async_accept

netexec::net::tls::async_accept
    ├── netexec::net::ip::tcp::async_accept
    └── TLS handshake

netexec::net::tls::async_send
    └── TLS encrypt
    └── netexec::net::ip::tcp::async_send (possibly in a loop)
    └── (future) framer

netexec::net::tls::async_receive
    └── netexec::net::ip::tcp::async_receive (possibly in a loop)
    └── TLS decrypt
    └── (future) framer
```

---

## Usage Example

```cpp
#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>

namespace ex  = stdexec;
namespace net = netexec::net;

// Client
auto remote = ex::env{
    net::endpoint_props::hostname{"example.org"},
    net::endpoint_props::port{std::uint16_t(443)}
};
net::tls::preconnection pre(remote);
auto conn = co_await net::tls::async_initiate(pre, ctx);
co_await net::tls::async_send(conn, msg);
auto reply = co_await net::tls::async_receive(conn);

// Server
auto local = ex::env{net::endpoint_props::port{12345}};
net::tls::preconnection pre(local);
auto srv = co_await net::tls::async_listen(pre, ctx);
auto client = co_await net::tls::async_accept(srv);
co_await net::tls::async_send(client, msg);
co_await net::tls::async_shutdown(client);
```
