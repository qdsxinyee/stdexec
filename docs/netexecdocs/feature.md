## 1. 构建与测试

Windows:
```bash
cmake --build build --config Release --target test.netexec
./build/test/netexec/Release/test.netexec.exe
```

Linux:
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DSTDEXEC_BUILD_TESTS=OFF
cmake --build build -j$(nproc)
```

### 1.1 更新 clangd 的 compile_commands.json

> 主构建目录是 `build/`（Visual Studio 生成器），用于实际编译和运行；`build-ninja/` 仅用于给 clangd 提供 `compile_commands.json`。Visual Studio 生成器不会生成 `compile_commands.json`，所以单独维护一个 Ninja 构建目录。

`.clangd` 配置使用 `build-ninja/compile_commands.json`。如果该文件不存在或过期，用 Ninja 重新生成：

```bash
cmake -S . -B build-ninja -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DSTDEXEC_BUILD_TESTS=OFF
```

仓库根目录的 `compile_commands.json` 是指向 `build-ninja/compile_commands.json` 的符号链接。

> **clangd 误报/崩溃**: 本项目依赖大量 `stdexec` 模板和协程代码，clangd 22 在解析某些文件（例如 `examples/netexec/http-server.cpp`）时可能出现崩溃或把红线标在 `sync_wait`/`spawn` 等位置。这属于 clangd 前端问题，不是代码错误。请以 `cmake --build build --config Release --target <target>` 的实际编译结果为准；如果 clangd 持续报错，可尝试更换 clangd 版本或在 `.clangd` 中进一步抑制相关诊断。

## 2. API 速查

### 2.1 所有网络操作都是 sender CPO

所有网络操作都是 sender CPO（Customization Point Object），符合 stdexec 概念：

```cpp
namespace netexec::net {

// tls 层 API
namespace tls {
    async_initiate;      // (preconnection, io_context) -> sender<stream>
    async_listen;        // (preconnection, io_context) -> sender<acceptor>
    async_accept;        // (acceptor) -> sender<stream>
    async_send;          // (stream, message) -> sender<void>
    async_receive;       // (stream) -> sender<message>
    async_shutdown;      // (stream) -> sender<void>
}

// socket API
namespace ip::tcp {
    async_connect;       // (socket, endpoint) -> sender<void>
    async_listen;        // (acceptor, endpoint) -> sender<void>
    async_accept;        // (acceptor) -> sender<socket, endpoint>
    async_send;          // (socket, buffer) -> sender<size_t>
    async_receive;       // (socket, buffer) -> sender<size_t>
    async_resolve;       // (io_context, hostname, port) -> sender<vector<endpoint>>
}

} // namespace netexec::net
```

每个 CPO 通过 `sender_cpo<Desc>` 实现，`Desc` 描述：
- 完成的签名（completion signature）
- 如何向 scheduler submit 操作
- 如何从 operation 结果构造返回值

### 2.2 普通 Socket API 示例

适合需要精细控制或自定义协议的场景。

```cpp
#include <netexec/net.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace net = netexec::net;
namespace tcp = netexec::net::ip::tcp;

auto echo(tcp::socket sock) -> exec::task<void> {
    char buf[64];
    while (true) {
        auto n = co_await tcp::async_receive(sock, net::buffer(buf));
        if (n == 0) co_return;
        co_await tcp::async_send(sock, net::buffer(buf, n));
    }
}

auto echo_client(net::io_context& ctx) -> exec::task<void> {
    tcp::endpoint ep(net::ip::address_v4::loopback(), 12345);
    tcp::socket sock(ctx, ep);

    co_await tcp::async_connect(sock);
    co_await tcp::async_send(sock, net::buffer("hello", 5));

    char buf[64];
    auto n = co_await tcp::async_receive(sock, net::buffer(buf));
    (void)n;
}

auto echo_server(net::io_context& ctx) -> exec::task<void> {
    tcp::endpoint ep(net::ip::address_v4::any(), 12345);
    tcp::acceptor acc(ctx, ep.protocol());

    // 显式 bind + listen，与 POSIX 语义一一对应
    co_await tcp::async_listen(acc, ep);

    ex::counting_scope scope;
    while (true) {
        auto [sock, client_ep] = co_await tcp::async_accept(acc);
        (void)client_ep;
        ex::spawn(echo(std::move(sock)) | ex::upon_error([](auto&&){}),
                  scope.get_token());
    }
}
```

### 2.3 tls 层 API 示例

适合希望声明式连接、显式启用 TLS 的场景。

```cpp
#include <netexec/net/tls.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

namespace ex  = stdexec;
namespace net = netexec::net;

auto taps_client(net::io_context& ctx) -> exec::task<void> {
    auto remote = ex::env{
        net::hostname("localhost"),
        net::port(12345),
    };
    net::tls::preconnection pre(remote);

    auto conn = co_await net::tls::async_initiate(pre, ctx);
    co_await net::tls::async_send(conn, net::message(std::string("hello")));
    auto reply = co_await net::tls::async_receive(conn);
}
```

### 2.4 服务端示例

```cpp
auto handle_client(net::tls::stream stream) -> exec::task<void> {
    auto request = co_await net::tls::async_receive(stream);
    (void)request;
    co_await net::tls::async_send(stream, net::message(std::string("hello")));
    co_await net::tls::async_shutdown(stream);  // TLS: 发送 close_notify
}

auto server(net::io_context& ctx) -> exec::task<void> {
    auto local = ex::env{
        net::endpoint_props::port{12345},
    };
    net::tls::preconnection pre(local);
    auto acc = co_await net::tls::async_listen(pre, ctx);

    ex::counting_scope scope;
    while (true) {
        auto client = co_await net::tls::async_accept(acc);
        ex::spawn(handle_client(std::move(client)) | ex::upon_error([](auto&&){}),
                  scope.get_token());
    }
}
```

`net::tls::async_accept` 从 `net::tls::acceptor` 取出一个新连接：内部先执行底层 `tcp::async_accept`，若 acceptor 启用了 TLS 则继续完成服务端握手，最后返回可直接收发数据的 `net::tls::stream`。连接处理结束时调用 `net::tls::async_shutdown` 发送 TLS `close_notify` 实现优雅关闭。

### 2.5 结构化并发

结合 `stdexec::counting_scope` 或 `stdexec::let_async_scope`：

```cpp
ex::counting_scope scope;
ex::spawn(server_work(ctx) | ex::upon_error([](auto&&){}), scope.get_token());
ex::spawn(ctx.async_run() | ex::upon_error([](auto&&){}), scope.get_token());
ex::sync_wait(scope.join());
```

## 3. 开发 HTTPS 证书方案

为便于本地 HTTPS 开发，`examples/netexec/` 提供了 `generate_dev_certs.py` 脚本，它生成一条自签名的根 CA + 服务器叶子证书链：

- `certs/ca.crt` / `certs/ca.key`：本地开发根 CA
- `certs/server.crt` / `certs/server.key`：由 CA 签名的服务器证书，SAN 包含 `localhost`、`127.0.0.1`、`::1`

使用流程：

1. 生成证书：
   ```bash
   python examples/netexec/generate_dev_certs.py
   ```

2. 把 `certs/ca.crt` 导入到运行浏览器的操作系统或浏览器信任存储（详见 [architecture.md](architecture.md) 中"开发证书与浏览器信任"一节）。

3. 启动信任证书的服务器示例：
   ```bash
   cd examples/netexec
   https-server-trusted.exe
   ```

4. 浏览器访问 `https://localhost:8443/`，不再提示自签名证书不安全。

`https-server-trusted.cpp` 通过 `net::tls::certificate(...)` 和 `net::tls::private_key(...)` 向 `net::tls::preconnection` 提供 PEM 文件路径；Schannel 后端在启动时加载这些文件并用于 TLS 握手。如果证书文件缺失或格式错误，服务器启动会失败，而不是回退到自签名证书。

## 4. 公网部署建议

对于公网部署，**不应该使用上述自签名 CA 方案**，而应该使用受公共信任的 CA 签发的证书，例如：

- **Let's Encrypt**：免费、自动化的 DV 证书，主流浏览器和操作系统均信任。
- 其他商业 CA（DigiCert、Sectigo 等）。

使用公共 CA 签发的证书时，客户端无需手动导入任何根证书即可建立可信 HTTPS 连接，避免用户体验和安全流程问题。

## 5. 设计说明与 TODO

### 5.1 消除本地开发证书警告（TODO）

> **注意**：证书加载 API 已经实现。`net::tls::preconnection::make_context()` 会调用后端的 `use_certificate_file`、`use_private_key_file` 和 `use_ca_bundle`；Schannel 后端也已完整支持 PEM 格式。当前 `https-server.cpp` 没有传入 `net::tls::certificate(...)` / `net::tls::private_key(...)`，因此走 Schannel 自动生成的内存自签名证书路径。如果传入受信任的叶子证书，浏览器不会警告。

当前 `https-server.cpp` 依赖 Schannel 运行时生成的自签名证书，Edge 等浏览器会报“不安全”，并可能因证书校验失败触发首次握手重试。后续需要实现以下两种方案之一，让本地 HTTPS 示例在浏览器中无警告、无重试：

1. **把自签名根证书导入 Windows 信任存储**
   - 在 Schannel 后端生成自签名证书时，同时导出对应的根证书（`.cer` / `.pem`）。
   - 提供 PowerShell 或证书管理器（`certmgr.msc`）导入说明，或示例脚本调用 `CertAddEncodedCertificateToStore` 将根证书写入 `ROOT` store。
   - 导入后，由该根证书签发的叶子证书会被系统/Edge 视为受信任，不再弹出警告。

2. **改用 OpenSSL 生成更规范的证书链**
   - 提供示例脚本（如 `generate_dev_certs.py` / `openssl.cnf` + shell 脚本），生成一条完整的开发证书链：根 CA → 中间 CA（可选）→ 叶子服务器证书。
   - `https-server.cpp` 通过 `net::tls::certificate("server.crt")` 和 `net::tls::private_key("server.key")` 显式加载叶子证书，而不是依赖 Schannel 运行时自签名。
   - 用户只需一次性把生成的根 CA 导入系统信任存储，即可获得长期有效的本地受信任 HTTPS 端点。

推荐最终形态：保留 Schannel 自动证书作为“零配置”兜底，同时在仓库里提供可选的 OpenSSL 证书生成脚本，方便需要无警告体验的用户。

### 5.2 async_shutdown API 设计（TODO）

当前 tls 层 API 中额外存在一个 `net::tls::async_shutdown(net::tls::stream&)`，用于在关闭 `net::tls::stream` 前向对端发送 TLS `close_notify`，实现 TLS 会话的优雅关闭。

**当前使用方式**

```cpp
auto stream = co_await net::tls::async_accept(acceptor);
// ... 收发数据 ...
co_await net::tls::async_shutdown(stream);  // TLS: 发送 close_notify；明文：no-op
// stream 析构时关闭底层 socket
```

**设计上的争议点**

1. `async_shutdown` 不在最初的 tls 层 API 设计之中（`async_initiate` / `async_listen` / `async_accept` / `async_send` / `async_receive` / `ip::tcp::*`）。
2. 它对明文 `net::tls::stream`（`secure=false`）是 no-op，因此只对 TLS 场景有意义。
3. 该 CPO 仅存在于 `netexec::net::tls` 命名空间，不会与普通 socket API 混淆。

**后续可能的替代方案**

- **方案 A：由 `net::tls::stream` 析构自动执行 shutdown**
  - 在 `net::tls::stream` 析构时，如果仍持有有效 TLS session，尝试同步发送 `close_notify` 后再关闭 socket。
  - 优点：用户无需显式调用，符合 RAII 直觉。
  - 难点：析构函数不能 `co_await`，同步 shutdown 可能阻塞事件循环；需要设计一套异步析构或 deferred close 机制。

- **方案 B：改为 `net::tls::stream` 的成员函数**
  - 例如 `stream.async_shutdown()` 或 `stream.close()`，不再是 `net` 命名空间下的独立 CPO。
  - 优点：语义上更贴近 stream 自身，不容易被误用为通用 API。
  - 缺点：需要暴露 `net::tls::stream` 上的 async 成员函数，与当前纯 CPO 风格不一致。

- **方案 C：由 scope / token 自动管理**
  - 连接绑定到 `netexec::scope` 或某个 stop token，离开作用域时由框架自动发送 `close_notify` 并关闭 socket。
  - 优点：与 `counting_scope` / `async_scope` 的并发模型一致。
  - 缺点：需要框架提供统一的“连接生命周期”抽象。

- **方案 D：保持现状**
  - 继续保留 `net::tls::async_shutdown` 作为显式优雅关闭 TLS 的 CPO。
  - 优点：实现简单，行为明确。
  - 缺点：API 表面多了一个非核心 CPO，与设计初衷不完全一致。

**结论**

目前暂时保留 `net::tls::async_shutdown`（方案 D），以满足 `https-server.cpp` 等示例对 TLS 优雅关闭的需求。未来根据 `net::tls::stream` 生命周期管理的设计，再决定是否迁移到方案 A/B/C，并在迁移完成后从公共 API 中移除 `net::tls::async_shutdown`。
