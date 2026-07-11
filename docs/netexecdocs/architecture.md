# netexec 架构与使用指南

## 1. 项目背景

`netexec` 是构建在 [`stdexec`](https://github.com/NVIDIA/stdexec) 之上的异步网络库，最初由 Dietmar Kühl 的 `beman/net` 移植而来。它目标是为 C++ 提供一套基于 sender/receiver 模型的异步网络 API，并探索 TAPS（Transport Services）风格的高层抽象。

## 2. 整体架构

netexec 采用**两层 API** 设计：

- **tls 层**：`netexec::net::tls::*`
  - `async_initiate` / `async_listen` / `async_accept`
  - `async_send` / `async_receive` / `async_shutdown`
  - TLS 显式启用、面向 message、基于 `preconnection`

- **socket API**：`netexec::net::ip::tcp::*`
  - `async_connect` / `async_listen` / `async_accept`
  - `async_send` / `async_receive` / `async_resolve`
  - 直接操作 socket/acceptor、裸 buffer、单次系统调用

- **底层实现**：`netexec::__detail::*`
  - `io_context` / scheduler / sender CPO
  - IOCP / poll / io_uring backend

### 2.1 命名空间

| 命名空间 | 层级 | 说明 |
|---|---|---|
| `netexec::net` | 入口 | 普通 socket API 与公共类型（`ip::tcp::*`、`io_context` 等） |
| `netexec::net::ip::tcp` | - | 直接 socket 操作 |
| `netexec::net::tls` | tls 层 | TLS 风格 API，TLS 显式启用 |
| `netexec::__detail` | 底层 | 实现细节，不直接暴露 |
| `netexec::net::tls::__detail` | 底层 | TLS 后端抽象与实现细节 |

## 3. 核心实现机制

### 3.1 io_context 与 scheduler

`netexec::io_context`（暴露为 `netexec::net::io_context`）是事件循环入口：

- 内部持有 `context_base` 平台抽象
- Windows 使用 IOCP backend
- Linux 使用 poll backend（可选 io_uring）
- 提供 `get_scheduler()` 返回 `io_context_scheduler`
- `async_run()` 启动事件循环 sender

```cpp
net::io_context ctx;
auto sched = ctx.get_scheduler();
```

各 API 的签名与使用示例见 [feature.md](feature.md)。

### 3.2 操作描述符（Desc）

以 `connect_desc` 为例：

```cpp
struct connect_desc {
    using operation = context_base::connect_operation;
    template <typename Socket>
    struct data {
        using completion_signature = stdexec::set_value_t();
        Socket& d_socket;
        auto id() const { return d_socket.id(); }
        auto events() const { return event_type::in_out; }
        auto get_scheduler() { return d_socket.get_scheduler(); }
        auto submit(auto* base) -> submit_result {
            std::get<0>(*base) = d_socket.get_endpoint();
            return d_socket.get_scheduler().connect(base);
        }
    };
};
```

### 3.3 状态机与生命周期

`sender_state` 继承自：
- `Desc::operation`：平台相关的 operation 状态
- `sender_state_base<Receiver>`：管理 receiver、stop token、outstanding 计数

关键设计：
- submit 后立即让 scheduler 接管，避免同步完成导致的 use-after-free
- stop token 通过 cancel callback 注册
- complete/cancel/stopped 路径由 scheduler 回调驱动

### 3.4 平台 backend

```
io_context
  └── context_base (abstract)
        ├── iocp_context  (Windows)
        ├── poll_context  (Linux default)
        └── uring_context (Linux optional)
```

backend 负责：
- 创建/释放 socket
- 注册 read/write/accept/connect/timer 事件
- 在事件就绪时调用 operation 的 completion handler

### 3.5 阻塞操作与后台线程池

`io_context` 的事件循环线程不能被阻塞调用卡住，否则所有连接都会停滞。但有些系统调用天生是阻塞的，典型例子是 **DNS 解析**（`getaddrinfo`）。netexec 通过在 `io_context` 内部维护一个 `exec::static_thread_pool`，把这些阻塞操作 offload 到后台线程。

#### 为什么放在 `io_context` 里

- `io_context` 是 netexec 的公共 API 入口，所有网络操作都需要它。
- 线程池生命周期与 `io_context` 绑定：构造时按需创建，析构时自动停止并 join。
- 不改变现有 API 签名：`net::tls::async_initiate(pre, ctx)` 和 `net::tls::async_listen(pre, ctx)` 无需额外参数，内部通过 `ctx.get_blocking_scheduler()` 获取 scheduler。

#### 主要用途

| 用途 | 说明 |
|---|---|
| **DNS 解析** | `net::ip::tcp::async_resolve` 在后台线程执行 `getaddrinfo`，结果返回给 IO 线程 |
| **证书/密钥文件加载** | 大文件或复杂格式解析可能阻塞，可放到后台线程 |
| **其他同步系统调用** | 用户可能注册的阻塞回调或同步文件 IO |

注意：TLS handshake、socket connect/read/write 不需要放到线程池，因为它们已经通过底层 IO backend（IOCP / poll / io_uring）异步驱动。

#### 实现选择：`exec::static_thread_pool`

stdexec 已经提供 `exec::static_thread_pool`（位于 `include/exec/static_thread_pool.hpp`）：

- header-only，无需额外依赖。
- 提供 P2300 scheduler，与 netexec 的 sender/receiver 模型直接集成。
- 默认线程数可使用硬件并发数，也可通过 `io_context` 配置。

示例用法：

```cpp
namespace ex = stdexec;

// 在后台线程执行 getaddrinfo，完成后恢复回 io_context scheduler
exec::task<std::vector<ip::tcp::endpoint>>
async_resolve(io_context& ctx, const std::string& hostname, std::uint16_t port) {
    co_await stdexec::schedule(ctx.get_blocking_scheduler());
    // ... 调用 getaddrinfo ...
    co_await stdexec::schedule(ctx.get_scheduler());
    co_return endpoints;
}
```

#### 生命周期与取消

- `io_context` 析构时，线程池自动 `request_stop()` 并 join 所有线程（RAII）。
- `getaddrinfo` 一旦开始执行就无法被取消：stop token 只能在任务开始前或任务结束后观察。这是线程池 offload 阻塞系统调用的固有限制；如果需要真正可取消的 DNS，未来要换用 `getaddrinfo_a`、c-ares 或自定义异步 resolver。

## 4. TLS 支持

### 4.1 后端选择

netexec 通过 `NETEXEC_TLS_BACKEND` CMake 选项选择 TLS 后端。Windows 默认使用 **Schannel**（系统原生，无需额外依赖），其他后端（OpenSSL、mbedTLS、SecureTransport）目前为占位实现。

| 平台 | 默认后端 | 状态 |
|---|---|---|
| Windows | Schannel | 完整实现 |
| Linux | OpenSSL | 占位（stub） |
| macOS | SecureTransport | 占位（stub） |

### 4.2 Schannel 后端架构

`include/netexec/net/tls/__detail/schannel_tls.hpp` 实现 Windows Schannel 封装：

- `schannel_tls_context`：加载证书、创建 credentials、创建 session
- `schannel_tls_session`：驱动 TLS 握手、加密/解密数据
- `generate_self_signed_certificate`：未提供证书时生成自签名证书

未提供 `net::tls::certificate` / `net::tls::private_key` 时，服务端会动态生成一张自签名证书，CN 和 SAN 取自 `net::hostname()`（默认 `localhost`）。为方便通过回环 IP 直接访问，SAN 还包含 `127.0.0.1` 与 `::1`。这样 `https://localhost:8443/`、`https://127.0.0.1:8443/` 与 `https://[::1]:8443/` 都能通过名称校验（仍会提示自签名证书不安全）。

### 4.3 TLS 1.2 与 TLS 1.3 的兼容性处理

Schannel 本身同时支持 TLS 1.2 和 TLS 1.3，但 netexec 的握手循环必须显式处理两种协议在状态机上的差异。

#### 4.3.1 握手状态码差异

| 状态码 | 含义 | TLS 1.2 | TLS 1.3 |
|---|---|---|---|
| `SEC_E_OK` | 握手完成 | ✅ | ✅ |
| `SEC_I_CONTINUE_NEEDED` | 还需要一轮握手数据 | ✅ | ✅ |
| `SEC_E_INCOMPLETE_MESSAGE` | 当前数据不足，需要再读 | ✅ | ✅ |
| `SEC_I_COMPLETE_NEEDED` | 需要调用 `CompleteAuthToken` | 极少 | 常见 |
| `SEC_I_INCOMPLETE_CREDENTIALS` | 需要补充凭据 | 客户端证书场景 | post-handshake 场景 |

TLS 1.3 的握手可能包含额外的 post-handshake 步骤（例如服务器发送 `NewSessionTicket`、请求 post-handshake 认证等），Schannel 会通过 `SEC_I_COMPLETE_NEEDED` 返回这些状态。netexec 的握手循环必须：

1. 在收到 `SEC_I_COMPLETE_NEEDED` 时调用 `CompleteAuthToken`
2. 继续驱动握手，直到 `SEC_E_OK`
3. 不要在中途清空 input buffer，因为 TLS 1.3 握手完成后可能紧跟着应用数据或 post-handshake 记录

#### 4.3.2 实际遇到的问题

在修复前，Edge 等现代浏览器默认优先 TLS 1.3 连接 `https://127.0.0.1:8443/`。由于 netexec 没有处理 `SEC_I_COMPLETE_NEEDED`，握手失败，浏览器显示 `ERR_TIMED_OUT`。而 curl/openssl 默认或显式使用 TLS 1.2 时则能成功。

修复后，netexec 的 Schannel 握手循环正确识别并处理 `SEC_I_COMPLETE_NEEDED` 与 `SEC_I_INCOMPLETE_CREDENTIALS`，同时保留 `SECBUFFER_EXTRA` 数据，使 TLS 1.3 握手能正常完成。

#### 4.3.3 当前限制

- 自签名证书仍会触发浏览器的安全警告，需手动点击"继续访问"。
- TLS 1.3 首次连接时，部分客户端可能因证书链校验问题先失败一次，随后自动重试成功。这是由于自签名证书不受系统信任导致的，不是协议层问题。
- 要彻底消除浏览器警告，需使用受信任 CA 签发的证书，或把自签名根证书导入系统信任存储。

#### 4.3.4 多 acceptor 下的证书生成

当服务端同时创建 IPv4 与 IPv6 两个 TLS acceptor（如 `https-server.cpp`）时，会构造两个独立的 TLS context，每个 context 按需生成自签名证书。为避免两个 context 在 Windows 密钥存储中使用同名容器、导致第二个 context 关联到错误私钥，Schannel 后端为每次证书生成使用唯一的密钥容器名（`tickcount + 递增计数器`）。这保证了 IPv4 与 IPv6 两个监听端点的 TLS 握手都能使用各自完整匹配的证书/私钥对。

#### 4.3.5 首次连接预热

自签名证书生成（2048-bit RSA 密钥对）和 `AcquireCredentialsHandle` 在首次调用时可能花费几百毫秒。`https-server.cpp` 在启动监听前主动调用 `create_server_session()` 对每个 TLS context 进行预热，把证书和 credentials 的创建开销从“第一个客户端连接时”转移到“服务器启动时”，从而避免浏览器/工具第一次访问时的明显卡顿。

### 4.4 开发证书与浏览器信任

`https-server-trusted.cpp` 演示了如何加载用户提供的 PEM 证书，而不是依赖 Schannel 动态生成的自签名证书。要消除浏览器的"不安全"警告，需要把生成证书时产生的根 CA（`certs/ca.crt`）导入到**运行浏览器的那台设备**的信任存储中。

不同平台/浏览器的证书存储位置不同，对应关系如下：

| 平台/浏览器 | 证书存储位置 | 是否需要单独导入 CA |
|---|---|---|
| Windows + Edge/Chrome | 系统证书存储 | 导入到系统即可 |
| Windows + Firefox | Firefox 自有存储 | 需要单独导入 |
| Linux + Chrome/Chromium | 系统 NSS/CA 目录 | 系统导入即可 |
| Linux + Firefox | Firefox 自有存储 | 需要单独导入 |
| macOS + Safari/Chrome | 系统钥匙串 | 系统导入即可 |
| macOS + Firefox | Firefox 自有存储 | 需要单独导入 |
| iOS / Android | 系统信任设置 | 需要单独安装 CA 证书 |

服务器本身只负责加载 `server.crt` 和 `server.key`；证书链的校验完全由客户端浏览器/操作系统完成。因此，**导入 `ca.crt` 的位置必须是运行浏览器的那台机器**。如果浏览器和服务器不在同一台机器上，需要把 `ca.crt` 复制到客户端并导入，同时服务器证书的 SAN 必须包含客户端实际访问时使用的 hostname 或 IP。

常用导入命令：

- **Windows（管理员 PowerShell）**
  ```powershell
  Import-Certificate -FilePath certs/ca.crt -CertStoreLocation Cert:\LocalMachine\Root
  ```

- **Linux（Debian/Ubuntu 等）**
  ```bash
  sudo cp certs/ca.crt /usr/local/share/ca-certificates/netexec-dev-ca.crt
  sudo update-ca-certificates
  ```

- **macOS**
  ```bash
  sudo security add-trusted-cert -d -r trustRoot \
    -k /Library/Keychains/System.keychain certs/ca.crt
  ```

使用示例见 [feature.md](feature.md)。

## 5. 文件组织

```
stdexec/include/netexec/
  net.hpp                        # 普通 socket API 总入口
  net/
    io_context.hpp               # net::io_context
    buffer.hpp                   # net::buffer
    timer.hpp                    # net::resume_after / resume_at
    properties.hpp               # env / hostname / port / endpoint_props / ...
    ip/
      address.hpp                # net::ip::address / address_v4 / address_v6
      tcp/
        socket.hpp               # socket / acceptor / endpoint
        operations.hpp           # socket CPO：async_connect / async_listen / async_accept / ...
  net/tls.hpp                    # tls 层总入口
  net/tls/
    preconnection.hpp            # net::tls::preconnection
    properties.hpp               # net::tls::secure / certificate / private_key / ca_bundle / ...
    stream.hpp                   # net::tls::stream / acceptor
    message.hpp                  # net::tls::message
    operations.hpp               # tls 层 CPO
    __detail/                    # TLS 后端抽象与实现
      context_base.hpp           # netexec::net::tls::__detail::context_base
      session_base.hpp           # netexec::net::tls::__detail::session_base
      schannel_tls.hpp           # Windows Schannel TLS 实现
      openssl_tls.hpp            # OpenSSL 占位
      ...
  __detail/                      # 底层实现
    io_context.hpp
    operations.hpp
    sender.hpp
    iocp_context.hpp / poll_context.hpp / uring_context.hpp
```

## 6. 当前限制

1. **TLS 后端不完整**：Windows Schannel 已可用；OpenSSL / SecureTransport / mbedTLS 仍为占位实现。
2. **Message 边界未实现**：`net::tls::async_receive` 简化为读到 EOF
3. **UDP 未实现**：仅有 TCP 支持
4. **rendezvous 占位**：抛 `not implemented`
