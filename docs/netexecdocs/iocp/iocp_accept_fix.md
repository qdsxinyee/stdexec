# IOCP `iocp_context.hpp` 多连接 accept 问题修复记录

> 修复目标：让 Windows IOCP 后端的 `http-server.cpp` 能够连续接受多个连接，而不是只能成功处理第一个连接。

## 1. 环境信息

- 后端宏：`NET_EXEC_USE_IOCP`
- 编译器：MSVC 19.51.36243 (x64)
- 测试程序：`stdexec/examples/netexec/http-server.cpp`、`accept_loop_test.cpp`
- 测试工具：`curl`、Python socket 客户端
- 链接库：`ws2_32.lib`

## 2. 现象

启动 `http-server.exe` 后：

- 第 1 个 `curl` 请求大概率超时。
- 即使第 1 个成功，第 2 个请求也几乎必定超时。
- 单独写一个最简的 `AcceptEx` 循环测试程序，**同一监听 socket 上连续 accept 是可以工作的**，说明问题不在 `AcceptEx` 本身，而在 `netexec` 对 `OVERLAPPED` 的管理层。

## 3. 逐步缩小问题范围

### 3.1 第一阶段：第一个连接就超时

**假设**：accept socket 在调用 `AcceptEx` 之前没有关联到 IOCP 端口。

`AcceptEx` 要求用于接收连接的 socket 必须先关联到同一个 IOCP 句柄，否则连接到达时 IOCP 不会投递完成包。

**验证与修复**：

在 `accept()` 里，创建 `accept_socket` 后、调用 `AcceptEx` 前，增加：

```cpp
::CreateIoCompletionPort(
    reinterpret_cast<HANDLE>(data->accept_socket),
    iocp_handle, 0, 0);
```

第一个连接随即能够成功。

### 3.2 第二阶段：第二个连接超时

第一个连接修复后，第二个连接又超时。

**假设 1**：`AcceptEx` 同步完成时没有正确处理。

Windows 下 `AcceptEx` 可能直接返回 `TRUE`，也可能返回 `FALSE` 并带 `ERROR_IO_PENDING`。如果返回 `TRUE`，也必须让 `run_one()` 能拿到完成包。

**修复**：把同步成功也视为已提交，增加 `outstanding` 计数，让 `GetQueuedCompletionStatus` 正常处理。

```cpp
if (ok || ::WSAGetLastError() == ERROR_IO_PENDING) {
    raw->result = ok ? static_cast<int>(bytes) : 0;
    ++outstanding;
    return submit_result::submit;
}
```

但第二个连接仍然超时。

### 3.3 第三阶段：排查 `make_socket` 时容器重分配

在 accept 完成回调里，会把 `data->accept_socket` 注册为新的 socket：

```cpp
::std::get<2>(*aop) = ctx.make_socket(...);
```

`make_socket` 可能会让内部的 `sockets` 容器重新分配，导致 `sockets[id].io` 指针失效。

**修复**：先把 `data->accept_socket` 保存到局部变量，再调用 `make_socket`。

```cpp
SOCKET accepted_socket = data->accept_socket;
data->accept_socket    = INVALID_SOCKET;
::std::get<2>(*aop)    = ctx.make_socket(static_cast<native_handle_type>(accepted_socket));
```

但这仍然没有解决第二个连接超时的问题。

### 3.4 第四阶段：移除多余的后关联

之前怀疑 accept 完成后需要再把已接受 socket 关联一次 IOCP。实际测试发现，对已关联的 socket 再次调用 `CreateIoCompletionPort` 会返回 `ERROR_INVALID_PARAMETER`，说明第一次关联已经足够。

**结论**：accept socket 只需要在 `AcceptEx` 前关联一次。

### 3.5 第五阶段：关键根因 —— 共用的 `OVERLAPPED`

回到 `iocp_socket_data` 结构：

```cpp
struct iocp_socket_data {
    iocp_accept_data accept_ol;
    iocp_io_data     recv_ol;
    iocp_io_data     send_ol;
    iocp_overlapped  connect_ol;
};
```

`accept_ol` 是监听 socket 数据的一部分，**所有 `AcceptEx` 操作都复用同一个 `accept_ol`**。

问题就在这里：

1. 第 1 个 `AcceptEx` 使用了 `sockets[id].io->accept_ol`，状态为 `PENDING`。
2. 在第一个连接尚未完成时，accept 循环再次调用 `async_accept`，触发第二个 `AcceptEx`。
3. 第二个 `AcceptEx` 初始化时，把同一个 `accept_ol` 重置为新的 `OVERLAPPED`，**覆盖了第一个 pending 操作的结构**。
4. 当第一个连接真正完成时，IOCP 投递的 `OVERLAPPED` 指针对应的内存已经被改写，第一个完成包丢失。
5. 后续连接全部乱序或超时。

**独立验证**：

写了一个最小化的 `iocp_accept_test.cpp`，直接调用 Windows API：

- 如果每次 `AcceptEx` 使用同一个 `OVERLAPPED` → 只能拿到一个完成。
- 如果每次 `AcceptEx` 使用独立分配的 `OVERLAPPED` → 可以连续接受多个连接。

这确认了根因。

## 4. 最终方案

### 4.1 每次 accept 分配独立的 `iocp_accept_data`

```cpp
auto data = ::std::make_unique<iocp_accept_data>();
data->completion    = op;
data->accept_socket = ::WSASocketW(family, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, WSA_FLAG_OVERLAPPED);
// ... 关联 IOCP ...
auto* raw = data.get();
op->extra = io_base::extra_t(raw, +[](void*) {}); // 等待期间由 context 持有
```

### 4.2 由 `iocp_context` 持有 pending 的 accept 状态

新增成员：

```cpp
::std::vector<::std::unique_ptr<iocp_accept_data>> kept_accept_data;

auto release_accept_data(iocp_accept_data* p) -> void {
    auto it = ::std::find_if(
        kept_accept_data.begin(), kept_accept_data.end(),
        [p](const auto& up) { return up.get() == p; });
    if (it != kept_accept_data.end()) {
        *it = ::std::move(kept_accept_data.back());
        kept_accept_data.pop_back();
    }
}
```

提交 `AcceptEx` 成功后：

```cpp
if (ok || ::WSAGetLastError() == ERROR_IO_PENDING) {
    raw->result = ok ? static_cast<int>(bytes) : 0;
    this->kept_accept_data.push_back(::std::move(data));
    ++outstanding;
    return submit_result::submit;
}
```

### 4.3 完成回调中释放

```cpp
op->work = [](context_base& ctx, io_base* io) -> submit_result {
    auto* data     = static_cast<iocp_accept_data*>(io->extra.get());
    auto& iocp_ctx = static_cast<iocp_context&>(ctx);
    auto cleanup   = [&]() { iocp_ctx.release_accept_data(data); };

    if (data->result < 0) {
        // 错误/取消处理 ...
        cleanup();
        return ...;
    }

    // 提取地址、更新 accept context、创建 socket ...
    io->complete();
    cleanup();
    return submit_result::ready;
};
```

这样：

- 每个 `AcceptEx` 有自己独立的 `OVERLAPPED`，不会被后续 accept 覆盖。
- 完成包到达后，`OVERLAPPED` 指向的内存仍然有效。
- 完成处理后立即从 `kept_accept_data` 中移除，避免内存堆积。

### 4.4 其它配套调整

- `run_one()` 只在 `GetQueuedCompletionStatus` 返回非空 `OVERLAPPED` 时才递减 `outstanding`，避免同步完成导致计数下溢。
- `run_one()` 对空的 `OVERLAPPED` 不再直接退出，而是检查是否还有 task / timeout / socket 在工作。
- `http-server.cpp` 恢复为原始版本，移除调试日志。

## 5. 验证结果

### 5.1 http-server

```bash
# 连续 50 次连接
for i in $(seq 1 50); do
  curl -s -o /dev/null -w "${i}:%{http_code}\n" http://127.0.0.1:12345/
done
```

结果：50 次全部返回 `200`。

并发 50 次 `curl`：全部返回 `200`。

### 5.2 accept_loop_test

用 Python 客户端连续发起 10 次连接：

```python
for i in range(10):
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(('127.0.0.1', 12347))
    s.close()
```

`accept_loop_test.exe` 正确接受了全部 10 个连接。

## 6. 经验总结

1. `AcceptEx` 的 `OVERLAPPED` 必须每个操作独立，不能在监听 socket 上复用同一个结构。
2. accept socket 必须在 `AcceptEx` 调用前关联到 IOCP 端口。
3. 同步完成的 IO 操作也需要让 `outstanding` 计数保持一致，否则 `run_one()` 会提前退出或下溢。
4. 独立的最小化测试程序（直接调用 WinSock API）对区分“API 本身问题”和“框架封装问题”非常有效。
5. 完成回调中访问的 `OVERLAPPED` 必须在 `GetQueuedCompletionStatus` 返回期间保持有效，因此 accept 状态的释放必须放在回调处理完之后。
