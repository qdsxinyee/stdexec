# tls layer Semantics and Mixing Examples

> Corresponding design: `netexec::net::tls::*` is the optional tls layer; `netexec::net::ip::tcp::*` is the socket API.

---

## Naming Convention

| Layer | Namespace | Naming Style | Semantics |
|---|---|---|---|
| tls layer | `netexec::net::tls::` | `async_*` | Complete/declarative semantics: resolution, TLS, framer, message |
| socket API | `netexec::net::ip::tcp::` | `async_*` | Raw/partial semantics: one system call, may be incomplete |

The two layers are distinguished by namespace. `netexec::net::tls::async_send` sends a complete message through a `tls::stream`, while `netexec::net::ip::tcp::async_send` issues a single raw `send` system call.

---

## Example 1: Correct Use of the tls layer

```cpp
#include <netexec/net/tls.hpp>

namespace ex  = stdexec;
namespace net = netexec::net;

auto remote = ex::env{
    net::endpoint_props::hostname{"example.org"},
    net::endpoint_props::port{std::uint16_t(443)}
};
net::tls::preconnection pre(remote);

// tls layer initiate: resolve address, establish connection, complete TLS handshake
auto conn = co_await net::tls::async_initiate(pre, ctx);

// tls layer send: send a complete message, possibly through framer/TLS
net::message msg{std::byte('h'), std::byte('i')};
co_await net::tls::async_send(conn, msg);

// tls layer receive: return a complete message
auto reply = co_await net::tls::async_receive(conn);
```

**Semantics**:
- `net::tls::async_initiate` is a high-level operation responsible for endpoint resolution, transport selection, and TLS handshake.
- `net::tls::async_send` guarantees sending a logically complete message.
- `net::tls::async_receive` guarantees returning a complete message (or an error), even if multiple underlying socket operations are needed.

---

## Example 2: Correct Use of the Socket API

```cpp
#include <netexec/net.hpp>

namespace tcp = netexec::net::ip::tcp;

tcp::socket sock(ctx);
tcp::endpoint ep(net::ip::address_v4::loopback(), 12345);

// Low-level connect: directly initiate a TCP connection
co_await tcp::async_connect(sock, ep);

// Low-level send: one system call, may only send part
char buf[] = "hello";
std::size_t total{};
while (total < sizeof(buf)) {
    auto n = co_await tcp::async_send(sock, net::buffer(buf + total, sizeof(buf) - total));
    total += n;
}

// Low-level receive: one system call, may only receive part
char recv_buf[1024];
auto n = co_await tcp::async_receive(sock, net::buffer(recv_buf));
```

**Semantics**:
- `tcp::async_connect` only performs the TCP three-way handshake, no TLS.
- `tcp::async_send` only issues one `send` system call and returns the number of bytes actually sent.
- `tcp::async_receive` only issues one `recv` system call and returns the number of bytes actually received.
- Looping until the full data is transferred is the user's responsibility.

---

## Example 3: Mixing — Calling Socket Operations on a tls Connection

```cpp
auto conn = co_await net::tls::async_initiate(pre, ctx);  // tls connection

char buf[1024];
// DO NOT do this: conn is a net::tls::stream, not a raw tcp::socket
auto n = co_await net::ip::tcp::async_receive(conn, net::buffer(buf));
```

**Problem**:
- This breaks the tls layer message semantics.
- `tcp::async_receive` only issues one system call and may return only part of a message.
- TLS is active, so reading the raw socket returns encrypted bytes and parsing will fail.

**Correct approach**:
```cpp
auto msg = co_await net::tls::async_receive(conn);  // use the tls layer receive
```

---

## Example 4: Mixing — Calling tls layer `async_send` on a Raw Socket

```cpp
namespace tcp = netexec::net::ip::tcp;

tcp::socket sock(ctx);
co_await tcp::async_connect(sock, ep);

// This will not compile: tcp::socket is not a net::tls::stream
net::message msg{std::byte('h')};
co_await net::tls::async_send(sock, msg);
```

**Problem**:
- A raw socket has no TLS stream context (TLS session, framer state, message context).
- The tls layer `async_send` requires a `net::tls::stream`.

**Correct approach**:
```cpp
co_await tcp::async_send(sock, net::buffer(msg.data(), msg.size()));
```

---

## Example 5: Explicit Cross-Layer Use

```cpp
auto conn = co_await net::tls::async_initiate(pre, ctx);

// Explicitly obtain the raw socket via socket() for low-level operations
auto& sock = conn.socket();
co_await net::ip::tcp::async_send(sock, net::buffer("raw bytes"));
```

**Note**:
- This is **allowed** because the user explicitly knows they are crossing layers.
- `socket()` is the explicit boundary: once called, you leave the tls abstraction and enter the raw socket domain.
- After crossing, the tls layer is no longer responsible for subsequent state.

---

## Summary

| Scenario | Recommended | Note |
|---|---|---|
| tls layer using tls CPOs | ✅ Recommended | Declarative, secure, message-oriented |
| socket API using TCP CPOs | ✅ Recommended | Precise control, manual byte-stream handling |
| tls connection calling TCP CPOs | ❌ Not recommended | Breaks message/TLS/framer semantics |
| Raw socket calling tls CPOs | ❌ Not recommended | Requires a `net::tls::stream` |
| Explicit cross-layer via `socket()` | ⚠️ Allowed | User explicitly takes responsibility |

---

## Design Goal

Distinguish the two API layers by namespace:

- `netexec::net::tls::async_send` — I want to send a **complete message** over TLS.
- `netexec::net::ip::tcp::async_send` — I want to issue one **raw send system call**.

The namespaces prevent most unintentional mixing.
