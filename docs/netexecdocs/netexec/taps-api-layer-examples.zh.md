# tls 层语义与混用示例

> 对应设计：`netexec::net::tls::*` 为可选的 tls 层，`netexec::net::ip::tcp::*` 为 socket API。

---

## 命名规则

| 层级 | 命名空间 | 命名风格 | 语义 |
|---|---|---|---|
| tls 层 | `netexec::net::tls::` | `async_*` | 完整/声明式语义：解析、TLS、framer、message |
| socket API | `netexec::net::ip::tcp::` | `async_*` | 原始/部分语义：一次系统调用，可能不完整 |

两层通过命名空间区分。`netexec::net::tls::async_send` 在 `tls::stream` 上发送完整 message，而 `netexec::net::ip::tcp::async_send` 只发起一次原始 `send` 系统调用。

---

## 示例 1：正确使用 tls 层

```cpp
#include <netexec/net/tls.hpp>

namespace ex  = stdexec;
namespace net = netexec::net;

auto remote = ex::env{
    net::endpoint_props::hostname{"example.org"},
    net::endpoint_props::port{std::uint16_t(443)}
};
net::tls::preconnection pre(remote);

// tls 层 initiate：根据 preconnection 解析地址、建立连接、完成 TLS 握手
auto conn = co_await net::tls::async_initiate(pre, ctx);

// tls 层 send：发送完整 message，可能经过 framer/TLS
net::message msg{std::byte('h'), std::byte('i')};
co_await net::tls::async_send(conn, msg);

// tls 层 receive：返回完整 message
auto reply = co_await net::tls::async_receive(conn);
```

**语义说明**：
- `net::tls::async_initiate` 是高层操作，负责 endpoint resolution、transport 选择、TLS 握手等。
- `net::tls::async_send` 保证发送逻辑上的完整 message。
- `net::tls::async_receive` 保证返回一个完整 message（或错误），即使底层需要多次 socket 操作。

---

## 示例 2：正确使用 socket API

```cpp
#include <netexec/net.hpp>

namespace tcp = netexec::net::ip::tcp;

tcp::socket sock(ctx);
tcp::endpoint ep(net::ip::address_v4::loopback(), 12345);

// 底层 connect：直接发起 TCP 连接
co_await tcp::async_connect(sock, ep);

// 底层 send：一次系统调用，可能只发一部分
char buf[] = "hello";
std::size_t total{};
while (total < sizeof(buf)) {
    auto n = co_await tcp::async_send(sock, net::buffer(buf + total, sizeof(buf) - total));
    total += n;
}

// 底层 receive：一次系统调用，可能只收一部分
char recv_buf[1024];
auto n = co_await tcp::async_receive(sock, net::buffer(recv_buf));
```

**语义说明**：
- `tcp::async_connect` 只负责 TCP 三次握手，不涉及 TLS。
- `tcp::async_send` 只保证发起一次 send 系统调用，返回实际发送字节数。
- `tcp::async_receive` 只保证发起一次 recv 系统调用，返回实际接收字节数。
- 循环发送/接收完整数据是用户自己的责任。

---

## 示例 3：混用 — 在 tls connection 上调用 socket 操作

```cpp
auto conn = co_await net::tls::async_initiate(pre, ctx);  // tls connection

char buf[1024];
// 不要这样做：conn 是 net::tls::stream，不是裸 tcp::socket
auto n = co_await net::ip::tcp::async_receive(conn, net::buffer(buf));
```

**问题**：
- 这破坏了 tls 层的 message 语义。
- `tcp::async_receive` 只读一次系统调用，可能只拿到半个 message。
- 当前 TLS 已启用，直接读裸 socket 会得到加密字节，解析会失败。

**正确做法**：
```cpp
auto msg = co_await net::tls::async_receive(conn);  // 用 tls 层的 receive
```

---

## 示例 4：在裸 socket 上调用 tls 层 `async_send`

```cpp
namespace tcp = netexec::net::ip::tcp;

tcp::socket sock(ctx);
co_await tcp::async_connect(sock, ep);

// 不能编译：tcp::socket 不是 net::tls::stream
net::message msg{std::byte('h')};
co_await net::tls::async_send(sock, msg);
```

**问题**：
- 裸 socket 上没有 TLS stream 上下文（TLS session、framer state、message context）。
- tls 层 `async_send` 要求参数是 `net::tls::stream`。

**正确做法**：
```cpp
co_await tcp::async_send(sock, net::buffer(msg.data(), msg.size()));
```

---

## 示例 5：合法但显式跨层

```cpp
auto conn = co_await net::tls::async_initiate(pre, ctx);

// 显式通过 socket() 拿到裸 socket，进行底层操作
auto& sock = conn.socket();
co_await net::ip::tcp::async_send(sock, net::buffer("raw bytes"));
```

**说明**：
- 这是**允许的**，因为用户明确知道自己正在跨层。
- `socket()` 是显式边界：一旦调用，你就离开了 TAPS 抽象，进入原始 socket 领域。
- 跨层后，tls 层对后续状态不再负责。

---

## 总结

| 场景 | 是否推荐 | 说明 |
|---|---|---|
| tls 层内部使用 tls CPO | ✅ 推荐 | 声明式、安全、message-oriented |
| socket API 使用 TCP CPO | ✅ 推荐 | 精确控制、手动处理字节流 |
| tls connection 上调用 TCP CPO | ❌ 不推荐 | 破坏 message/TLS/framer 语义 |
| 裸 socket 上调用 tls CPO | ❌ 不推荐 | 需要 `net::tls::stream` |
| 通过 `socket()` 显式跨层 | ⚠️ 允许 | 用户明确承担责任 |

---

## 设计目标

通过命名空间区分两层 API：

- `netexec::net::tls::async_send` — 我要通过 TLS 发送一个**完整 message**。
- `netexec::net::ip::tcp::async_send` — 我要做一次**原始 send 系统调用**。

命名空间本身就能防止大部分无意识的混用。
