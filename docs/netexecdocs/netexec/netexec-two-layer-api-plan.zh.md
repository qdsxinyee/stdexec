# netexec 两层 API 重构计划

## 目标

将现有 `netexec::*` 的 socket API 重构为两层结构：

- **`netexec::net::ip::tcp::*`**：直接操作 socket/acceptor 的裸 buffer API，从现有 `netexec::*` 迁移。操作名已简化为 `async_connect`、`async_listen`、`async_accept`、`async_send`、`async_receive`，分别对应一次系统调用级别的语义。
- **tls 层 `netexec::net::tls::*`**：面向 TLS 的高层 API，基于 `preconnection` 和 `message`，TLS 显式启用。

`netexec::net::ip::tcp::*` 提供普通 socket 操作；需要 TLS 语义时，通过 `#include <netexec/net/tls.hpp>` 使用 `netexec::net::tls::*`。

底层实现继续放在 `netexec::__detail/*` 与 `netexec::net::tls::__detail/*`，只把 public 入口换到对应命名空间下。

---

## 最终 API 结构

```cpp
namespace netexec::net {

namespace ip::tcp {

// socket CPO（裸 buffer，单次系统调用）
using socket   = ::netexec::ip::tcp::socket;
using acceptor = ::netexec::ip::tcp::acceptor;
using endpoint = ::netexec::ip::tcp::endpoint;

inline constexpr struct async_connect_t  { ... } async_connect{};   // socket -> endpoint
inline constexpr struct async_listen_t   { ... } async_listen{};    // acceptor -> endpoint (bind + listen)
inline constexpr struct async_accept_t   { ... } async_accept{};    // acceptor -> (socket, endpoint)
inline constexpr struct async_send_t     { ... } async_send{};      // partial send
inline constexpr struct async_receive_t  { ... } async_receive{};   // partial receive
inline constexpr struct async_resolve_t  { ... } async_resolve{};   // io_context, hostname, port -> endpoints

} // namespace ip::tcp

} // namespace netexec::net

namespace netexec::net::tls {

class preconnection;      // client_connection / server_connection 就是它
class stream;             // TLS 包装后的连接（原 tls_stream）
class acceptor;           // TLS 监听端点（原 tls_acceptor）
class message;

// tls 层 CPO（面向 message，TLS 显式启用）
inline constexpr struct async_initiate_t { ... } async_initiate{};
inline constexpr struct async_listen_t   { ... } async_listen{};
inline constexpr struct async_accept_t   { ... } async_accept{};
inline constexpr struct async_send_t     { ... } async_send{};
inline constexpr struct async_receive_t  { ... } async_receive{};
inline constexpr struct async_shutdown_t { ... } async_shutdown{};

// 安全属性
inline constexpr struct secure_t            { ... } secure{};
inline constexpr struct certificate_t       { ... } certificate{};
inline constexpr struct private_key_t       { ... } private_key{};
inline constexpr struct ca_bundle_t         { ... } ca_bundle{};
inline constexpr struct use_system_trust_store_t { ... } use_system_trust_store{};

} // namespace netexec::net::tls
```

---

## 新增 tls 层头文件

| 文件 | 内容 |
|---|---|
| `stdexec/include/netexec/net/tls.hpp` | 总入口：include `net/tls/operations.hpp`、`net/tls/preconnection.hpp`、`net/tls/properties.hpp` 等 |
| `stdexec/include/netexec/net/tls/operations.hpp` | tls 层 6 个 CPO：`async_initiate`、`async_listen`、`async_accept`、`async_send`、`async_receive`、`async_shutdown` |
| `stdexec/include/netexec/net/tls/preconnection.hpp` | `netexec::net::tls::preconnection` 类，支持从 `env` + properties 构造 |
| `stdexec/include/netexec/net/tls/properties.hpp` | `endpoint_props::hostname/port/ip_address`、`security_props::secure`、`tls::certificate`、`tls::private_key` 等 |
| `stdexec/include/netexec/net/tls/stream.hpp` | `netexec::net::tls::stream` / `acceptor` 类型 |
| `stdexec/include/netexec/net/tls/message.hpp` | `netexec::net::tls::message` 类（与 `netexec::net::message` 共享） |

## 新增 socket 头文件

| 文件 | 内容 |
|---|---|
| `stdexec/include/netexec/net/ip/tcp/operations.hpp` | 6 个 socket CPO：`async_connect`、`async_listen`、`async_accept`、`async_send`、`async_receive`、`async_resolve` |
| `stdexec/include/netexec/net/ip/tcp/socket.hpp` | `using socket = ::netexec::ip::tcp::socket;` 等类型暴露 |

## 修改现有头文件

| 文件 | 修改 |
|---|---|
| `stdexec/include/netexec/net.hpp` | 总入口：include 新的 `net/ip/tcp/operations.hpp` 等；不再直接暴露旧的 TAPS 骨架；TLS/TAPS 入口改由 `<netexec/net/tls.hpp>` 提供 |
| `stdexec/include/netexec/net/tls/__detail/...` | TLS 抽象与后端实现：抽象类改为 `netexec::net::tls::__detail::context_base` / `session_base`；后端文件位于 `include/netexec/net/tls/__detail/` |

## 更新示例

| 文件 | 更新内容 |
|---|---|
| `stdexec/examples/netexec/taps.cpp` | 改用 tls 层 API：`net::tls::preconnection` 用 env 构造，`net::tls::async_initiate/send/receive`，`stdexec::counting_scope` 替代 `net::scope` |
| `stdexec/examples/netexec/server.cpp` | socket API 示例：`netexec::net::ip::tcp::async_accept/async_send/async_receive` |
| `stdexec/examples/netexec/client.cpp` | socket API 示例：`netexec::net::ip::tcp::async_connect/async_send/async_receive` |
| `stdexec/examples/netexec/https-server.cpp` | TLS 服务端示例：`net::tls::preconnection`、`net::tls::async_listen`、`net::tls::async_accept`、`net::tls::async_shutdown` |
| 其他示例 | 相应替换命名空间和 CPO 名 |

## 更新测试

| 文件 | 更新内容 |
|---|---|
| `stdexec/test/netexec/test_helpers.hpp` | `namespace net = netexec;` → `namespace net = netexec::net;` |
| `stdexec/test/netexec/test_netexec_*.cpp` | socket API 调用改为 `net::ip::tcp::async_*`；服务端示例改为先创建 `acceptor`，再 `co_await ip::tcp::async_listen(acc, ep)`，然后 `ip::tcp::async_accept(acc)` |

---

## 两层调用关系

```cpp
// tls 层
net::tls::async_initiate(client_pre, ctx)
    └── 内部：net::ip::tcp::async_resolve (如需要)
        └── net::ip::tcp::async_connect
        └── TLS 握手

net::tls::async_listen(server_pre, ctx)
    └── 内部：net::ip::tcp::acceptor(ctx, ep.protocol())
        └── co_await net::ip::tcp::async_listen(acc, ep)  // 显式 bind + listen

net::tls::async_accept(server_acc)
    └── 内部：co_await net::ip::tcp::async_accept(raw_acc)
        └── TLS 服务端握手

net::tls::async_send(conn, msg)
    └── 内部：TLS 加密 + 循环 net::ip::tcp::async_send

net::tls::async_receive(conn)
    └── 内部：循环 net::ip::tcp::async_receive + TLS 解密 + framer
```

---

## 示例代码

### tls 层 Client

```cpp
#include <netexec/net/tls.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace ex = stdexec;
namespace net = netexec::net;

auto client(net::io_context& ctx) -> exec::task<void> {
    auto remote = ex::env{
        net::endpoint_props::hostname{"example.com"},
        net::endpoint_props::port{std::uint16_t(443)}
    };
    net::tls::preconnection client_pre(remote);

    auto conn = co_await net::tls::async_initiate(client_pre, ctx);
    co_await net::tls::async_send(conn, net::message(std::string("hello")));
    auto reply = co_await net::tls::async_receive(conn);
}
```

### 普通 socket Server

```cpp
#include <netexec/net.hpp>

namespace ex = stdexec;
namespace net = netexec::net;
namespace tcp = netexec::net::ip::tcp;

auto handle_client(tcp::socket client) -> exec::task<void> {
    char buf[1024];
    for (std::size_t n; (n = co_await tcp::async_receive(client, net::buffer(buf)));) {
        co_await tcp::async_send(client, net::buffer(buf, n));
    }
}

auto server(net::io_context& ctx) -> exec::task<void> {
    tcp::endpoint ep(net::ip::make_address("0.0.0.0"), 12345);
    tcp::acceptor acc(ctx, ep.protocol());
    co_await tcp::async_listen(acc, ep);

    ex::counting_scope scope;
    while (true) {
        auto [client, addr] = co_await tcp::async_accept(acc);
        (void)addr;
        ex::spawn(handle_client(std::move(client)) | ex::upon_error([](auto&&){}),
                  scope.get_token());
    }
}
```

---

## 实施步骤

由于改动面大，建议分阶段：

1. **Phase 1：建立 socket 命名空间** ✅ 已完成
   - 新增 `netexec/net/ip/tcp/operations.hpp` 和 `socket.hpp`
   - 让 `netexec::net::ip::tcp::*` 可用
   - 暂时保留旧 `netexec::*` 不删，保证编译不炸
   - MSVC `cl` 语法检查通过

2. **Phase 2：测试/示例迁移到 socket API** ✅ 已完成
   - 把 `net::async_*` 改为 `net::ip::tcp::async_*`
   - 所有示例和测试文件更新为 `namespace net = netexec::net;`
   - `net::scope` 改为 `netexec::scope`
   - 新增 `netexec/net/timer.hpp` 暴露 `resume_after` / `resume_at`
   - `cmake --build build --config Release --target test.netexec` 成功
   - `./build/test/netexec/Release/test.netexec.exe`：28 个 test case，45 个 assertion 全部通过

3. **Phase 3：实现 tls 层 API** ✅ 已完成
   - 新增 `netexec/net/preconnection.hpp`、`properties.hpp`、`message.hpp`
   - 实现 `async_initiate`、`async_listen`、`async_send`、`async_receive`
   - `async_rendezvous` / `async_resolve` 先占位（抛 `not implemented`）
   - 属性用 `stdexec::env` + `stdexec::prop` + `stdexec::__query` 实现
   - `async_send` 循环 `async_send` 保证完整发送
   - `async_receive` Phase 3 简化为读到 EOF，未来加 framer 后按 message 边界
   - `cmake --build build --config Release --target test.netexec` 成功
   - 测试全部通过：28 test cases，45 assertions

4. **Phase 4：更新 tls 层示例** ✅ 已完成
   - 重写 `taps.cpp` 用 tls 层 API
   - 使用 `ex::env{net::hostname(...), net::port(...), net::secure(false)}` 构造 `preconnection`
   - 使用 `net::async_initiate`、`net::async_send`、`net::async_receive`
   - （后续已在 Phase 6 中移入 `netexec::net::tls`）
   - `secure(false)` 因为 Phase 3 尚未实现 TLS
   - 保留 `server.cpp` 等作为 socket 示例
   - MSVC `cl` 编译 `taps.cpp` 通过

5. **Phase 5：清理旧 public API** ✅ 已完成
   - 从 `netexec/net.hpp` 中移除旧的 TAPS 骨架 include：
     - `__detail/initiate.hpp`
     - `__detail/listen.hpp`
     - `__detail/local_endpoint.hpp`
     - `__detail/preconnection.hpp`
     - `__detail/remote_endpoint.hpp`
     - `__detail/rendezvous.hpp`
   - `netexec/net.hpp` 现在只暴露 `netexec::net::*` 普通 socket API
   - 底层实现 `netexec::__detail/*` 保留，`net::ip::tcp::*` 继续通过 `using` 转发
   - `cmake --build build --config Release --target test.netexec` 成功
   - 关键示例 `netexec.taps`、`netexec.server`、`netexec.client` 等构建成功
   - 测试全部通过：28 test cases，45 assertions

6. **Phase 6：将 tls 层 API 移入 `netexec::net::tls`** ✅ 已完成
   - 新增 `netexec/net/tls.hpp` 作为 tls 层总入口
   - 将 `net::preconnection`、`net::async_initiate`、`net::async_listen`、`net::async_accept`、`net::async_send`、`net::async_receive`、`net::async_shutdown` 移入 `netexec::net::tls`
   - 类型 `tls_stream` / `tls_acceptor` 改名为 `net::tls::stream` / `net::tls::acceptor`
   - 安全属性 `net::certificate`、`net::private_key`、`net::ca_bundle`、`net::use_system_trust_store` 移入 `netexec::net::tls`
   - TLS 后端抽象类改名为 `netexec::net::tls::__detail::context_base` / `session_base`
   - TLS 后端文件从 `include/netexec/__detail/tls/` 迁移到 `include/netexec/net/tls/__detail/`
   - 低层 socket CPO 名简化：`async_bind_listen` → `async_listen`、`async_accept_raw` → `async_accept`、`async_send_some` → `async_send`、`async_receive_some` → `async_receive`
   - `netexec::net::*` 不再默认开启 TLS，TLS 通过 `netexec::net::tls::*` 显式启用
   - 更新相关文档与示例
