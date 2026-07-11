# std::net TLS 拆分计划

目标：把当前 `feature/net` 中内建在 `netexec::net` 里的 TLS 能力拆分出去，放到独立的 `netexec::net::tls` 命名空间中；`netexec::net` 回归为最朴素的 socket 操作层。

---

## 第 0 步：确认当前分支状态

当前在 `feature/net` 分支，工作区包含：
- IOCP `tasks` 链表竞争修复（`include/netexec/__detail/iocp_context.hpp`）
- `test_let_async_scope.cpp` MSVC Debug C4700 修复
- TLS 集成过程中新增/删除/修改的文件
- 未跟踪文件：`include/netexec/net/`、`include/netexec/__detail/tls/`、`examples/netexec/https-server*.cpp`、`examples/netexec/certs/` 等

这些现行代码需要先固化成 baseline。

---

## 第 1 步：在 `feature/net` 提交 baseline

```bash
git add -A
git commit -m "WIP: baseline before splitting TLS into netexec::net::tls"
```

把所有当前改动打包成一个 commit，作为后续拆分的起点。

---

## 第 2 步：创建 `default-tls` 分支并推送

```bash
git branch default-tls
git push -u origin default-tls
```

`default-tls` 完整保留“TLS 内建在 `netexec::net` 里”的现行实现，作为对比和回退基准。

---

## 第 3 步：在 `feature/net` 上重构

### 3.1 目录和文件搬迁

新建目录结构：

```
include/netexec/net/tls/
├── tls.hpp
├── preconnection.hpp
├── stream.hpp
├── acceptor.hpp
├── context.hpp          # 证书、CA、system_trust_store 等
├── operations.hpp       # tls::async_initiate / async_listen / async_accept / async_send / async_receive
└── __detail/
    ├── tls_context_base.hpp
    ├── tls_session_base.hpp
    ├── run_handshake.hpp
    └── platform/        # Schannel / OpenSSL / Secure Transport 后端
```

具体搬迁：
- `include/netexec/net/tls_stream.hpp` → `include/netexec/net/tls/stream.hpp`
- `include/netexec/net/tls_acceptor.hpp` → `include/netexec/net/tls/acceptor.hpp`
- `include/netexec/__detail/security_props.hpp` → `include/netexec/net/tls/context.hpp`（或 `__detail/security_props.hpp`）
- `include/netexec/__detail/tls/` → `include/netexec/net/tls/__detail/`

### 3.2 命名空间调整

| 原位置 | 新位置 |
|---|---|
| `netexec::net::tls_stream` | `netexec::net::tls::stream` |
| `netexec::net::tls_acceptor` | `netexec::net::tls::acceptor` |
| `netexec::net::preconnection` | `netexec::net::tls::preconnection` |
| `netexec::net::async_initiate` | `netexec::net::tls::async_initiate` |
| `netexec::net::async_listen`（TAPS 版） | `netexec::net::tls::async_listen` |
| `netexec::net::async_accept`（TAPS 版） | `netexec::net::tls::async_accept` |
| `netexec::net::async_send`（TLS） | `netexec::net::tls::async_send` |
| `netexec::net::async_receive`（TLS） | `netexec::net::tls::async_receive` |
| `netexec::__detail::tls_context_base` | `netexec::net::tls::__detail::context_base` |
| `netexec::__detail::tls_session_base` | `netexec::net::tls::__detail::session_base` |
| `netexec::net::__detail::run_tls_handshake` | `netexec::net::tls::__detail::run_handshake` |

### 3.3 核心 socket 层 API 归位到 `netexec::ip::tcp`

```cpp
namespace netexec::ip::tcp {

async_connect(socket&, endpoint);
async_listen(acceptor&, endpoint);        // 原 async_bind_listen
async_accept(acceptor&);                  // 原 async_accept_raw
async_send(socket&, buffer);              // 原 async_send_some
async_receive(socket&, buffer);           // 原 async_receive_some
async_resolve(io_context&, hostname, port);

} // namespace netexec::ip::tcp
```

`netexec::net` 核心只保留类型聚合（`net::ip::tcp::socket`、`net::ip::tcp::acceptor` 等）和基础工具（buffer、message、io_context、scope、timer）。

### 3.4 拆分 `include/netexec/net/operations.hpp`

- 删除其中所有 TLS 相关逻辑（`run_tls_handshake`、`secure()` 分支、TLS handshake 等）。
- 在 `include/netexec/net/tls/operations.hpp` 里重建 TLS 操作。
- `tls::async_initiate` 内部调用：
  - `net::ip::tcp::async_resolve(ctx, hostname, port)`
  - `net::ip::tcp::async_connect(socket, endpoint)`
  - `tls::__detail::run_handshake(socket, session)`

### 3.5 更新总头文件

- `include/netexec/net.hpp`：只 include 核心 socket 头文件，不再 include TLS。
- 新增 `include/netexec/net/tls.hpp`：作为 `netexec::net::tls` 的总入口。

### 3.6 更新测试

- `test/netexec/test_netexec_tls.cpp`：全部改用 `netexec::net::tls`。
- `test/netexec/test_netexec_resolve.cpp`：改用 `netexec::ip::tcp::async_resolve(ctx, hostname, port)`，不再依赖 `preconnection` 和 `secure(false)`。
- 检查其他测试，移除任何误用的 TLS 属性或 `secure(false)`。

### 3.7 更新示例

- `examples/netexec/https-server.cpp`
- `examples/netexec/https-server-trusted.cpp`
- 任何用到 `preconnection`、`tls_stream`、`tls_acceptor` 的示例

全部改用 `netexec::net::tls::preconnection`、`netexec::net::tls::stream` 等。

---

## 第 4 步：分阶段提交

每完成一大块就提交一次：

1. `move tls public headers to netexec::net::tls`
2. `move tls backend internals to netexec::net::tls::__detail`
3. `split tls operations from net::operations`
4. `rename low-level socket ops: async_listen, async_accept, async_send, async_receive`
5. `update tests for tls namespace`
6. `update examples for tls namespace`

---

## 第 5 步：验证

- Debug 编译 `test.netexec` 并全量运行。
- Release 编译 `test.netexec` 并全量运行。
- 尝试编译 TLS 示例（https-server 等）。

目标是 Debug/Release 下 `test.netexec` 全部通过。
