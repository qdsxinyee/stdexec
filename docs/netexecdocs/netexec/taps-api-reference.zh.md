# `netexec::net::tls` API 参考

> 本说明基于最终方案 B：tls 层 API 位于 `netexec::net::tls`，使用 `async_*` 前缀；socket API 位于 `netexec::net::ip::tcp`，单次系统调用操作也使用 `async_*` 前缀。

---

## 命名空间层级

```cpp
netexec::net::tls::*            // 可选的 tls 层（声明式、面向 message、TLS 显式启用）
netexec::net::ip::tcp::*        // 底层 socket API（原始、部分字节、手动控制）
```

`netexec::net::ip::tcp::*` 提供普通 TCP socket 操作；tls 层通过 `netexec::net::tls::*` 显式启用。

---

## tls 层 API

```cpp
namespace netexec::net::tls {

// 根据 preconnection 主动建立连接。
// 内部依赖：
//   - netexec::net::ip::tcp::async_resolve   （当 preconnection 使用 hostname 时）
//   - netexec::net::ip::tcp::async_connect   （完成 TCP 三次握手）
//   - TLS 握手                             （当 preconnection 启用安全连接时）
inline constexpr struct async_initiate_t { ... } async_initiate{};
// 签名：async_initiate(const preconnection&, io_context&) -> sender<stream>

// 根据 preconnection 创建监听端点。
// 内部依赖：
//   - 创建 netexec::net::ip::tcp::acceptor
//   - acceptor.bind(endpoint)
//   - acceptor.listen()
// 后续通常配合：
//   - netexec::net::tls::async_accept   （接收一个 tls::stream）
inline constexpr struct async_listen_t { ... } async_listen{};
// 签名：async_listen(const preconnection&, io_context&) -> sender<acceptor>

// 接收一个新的 TLS 连接。
// 内部依赖：
//   - netexec::net::ip::tcp::async_accept   （接收裸 tcp::socket）
//   - TLS 握手                            （当 acceptor 启用安全连接时）
inline constexpr struct async_accept_t { ... } async_accept{};
// 签名：async_accept(acceptor&) -> sender<stream>

// 发送完整 message。
// 内部依赖：
//   - 可能循环调用 netexec::net::ip::tcp::async_send
//   - 可能经过 framer 编码
//   - 经过 TLS 加密包装
inline constexpr struct async_send_t { ... } async_send{};
// 签名：async_send(stream&, const message&) -> sender<void>

// 接收完整 message。
// 内部依赖：
//   - 可能循环调用 netexec::net::ip::tcp::async_receive
//   - 可能经过 framer 解码
//   - 经过 TLS 解密拆包
inline constexpr struct async_receive_t { ... } async_receive{};
// 签名：async_receive(stream&) -> sender<message>

// 优雅关闭 TLS 连接，发送 close_notify。
// 对纯 TCP stream 为 no-op。
inline constexpr struct async_shutdown_t { ... } async_shutdown{};
// 签名：async_shutdown(stream&) -> sender<void>

} // namespace netexec::net::tls
```

---

## 连接类型

```cpp
namespace netexec::net::tls {

class stream { /* ... */ };    // TLS 包装后的连接
class acceptor { /* ... */ };  // TLS 监听端点

} // namespace netexec::net::tls
```

---

## 普通 socket API

```cpp
namespace netexec::net::ip::tcp {

// 解析主机名到端点。
// 签名：async_resolve(io_context&, const std::string& hostname, std::uint16_t port)
//            -> sender<std::vector<endpoint>>
inline constexpr struct async_resolve_t { ... } async_resolve{};

// 发起一次 TCP 连接（一次系统调用级别的异步 connect）
inline constexpr struct async_connect_t { ... } async_connect{};
// 签名：async_connect(socket&, const endpoint&) -> sender<void>

// 在 acceptor 上执行 bind + listen（显式 bind + listen）
inline constexpr struct async_listen_t { ... } async_listen{};
// 签名：async_listen(acceptor&, const endpoint&) -> sender<void>

// 接受一个 TCP 连接（一次系统调用级别的异步 accept）
inline constexpr struct async_accept_t { ... } async_accept{};
// 签名：async_accept(acceptor&) -> sender<socket, endpoint>

// 发送一部分字节（可能不完整）
inline constexpr struct async_send_t { ... } async_send{};
// 签名：async_send(socket&, const_buffer) -> sender<size_t>

// 接收一部分字节（可能不完整）
inline constexpr struct async_receive_t { ... } async_receive{};
// 签名：async_receive(socket&, mutable_buffer) -> sender<size_t>

} // namespace netexec::net::ip::tcp
```

---

## 依赖关系图

```
netexec::net::tls::async_initiate
    ├── netexec::net::ip::tcp::async_resolve
    ├── netexec::net::ip::tcp::async_connect
    └── TLS 握手

netexec::net::tls::async_listen
    ├── netexec::net::ip::tcp::acceptor::bind
    └── netexec::net::ip::tcp::acceptor::listen
    └── （配合） netexec::net::tls::async_accept

netexec::net::tls::async_accept
    ├── netexec::net::ip::tcp::async_accept
    └── TLS 握手

netexec::net::tls::async_send
    └── TLS 加密
    └── netexec::net::ip::tcp::async_send （可能循环）
    └── （未来） framer

netexec::net::tls::async_receive
    └── netexec::net::ip::tcp::async_receive （可能循环）
    └── TLS 解密
    └── （未来） framer
```

---

## 使用示例

```cpp
#include <netexec/net.hpp>
#include <netexec/net/tls.hpp>

namespace ex  = stdexec;
namespace net = netexec::net;

// 客户端
auto remote = ex::env{
    net::endpoint_props::hostname{"example.org"},
    net::endpoint_props::port{std::uint16_t(443)}
};
net::tls::preconnection pre(remote);
auto conn = co_await net::tls::async_initiate(pre, ctx);
co_await net::tls::async_send(conn, msg);
auto reply = co_await net::tls::async_receive(conn);

// 服务端
auto local = ex::env{net::endpoint_props::port{12345}};
net::tls::preconnection pre(local);
auto srv = co_await net::tls::async_listen(pre, ctx);
auto client = co_await net::tls::async_accept(srv);
co_await net::tls::async_send(client, msg);
co_await net::tls::async_shutdown(client);
```
