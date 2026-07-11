# IOCP HTTP 服务器连接重置问题修复记录

> 注：本文档记录的问题发生在 API 重构之前。文中所用的 `net::async_receive` 等 TAPS 层 API 已迁移到 `netexec::net::tls::async_receive`。

## 现象

在 Windows 上启用 IOCP 后端（定义 `NET_EXEC_USE_IOCP`）编译并运行 `netexec.http-server.exe`：

- 服务器能监听、能接收连接。
- 客户端（Python socket / curl）会报 `ConnectionResetError 10054` 或 curl exit 56。
- 响应体没有完整送达。

同样的代码在默认的 `poll_context`（WSAPoll）后端上可以正常工作。

## 环境

- 后端：`iocp_context.hpp`（方案 B：非阻塞 socket + `SetFileCompletionNotificationModes` + 同步完成 inline 处理）。
- 编译器：MSVC 19.51.36243 (x64)。
- 系统：Windows 10.0.26200。

## 根因

问题出在 `iocp_context.hpp` 中 `receive()` 与 `send()` 的**同步完成**路径。

启用 `FILE_SKIP_COMPLETION_PORT_ON_SUCCESS` 后，`WSARecv` / `WSASend` 可能会直接同步成功（返回 `0`）。此时不会向 IOCP 投递完成包，必须由我们 inline 处理完成。

原来的同步成功代码：

```cpp
if (rc == 0) {
    data.result = static_cast<int>(bytes);
    op->complete();          // ❌ 直接完成，跳过了 work lambda
    return submit_result::ready;
}
```

`op->complete()` 只负责减少 `d_outstanding` 并唤醒 receiver，**没有**把 `data.result` 拷贝到操作状态 `std::get<2>(*op)` 中。

而真正负责把字节数写进 `std::get<2>(*op)` 的是 `op->work` lambda：

```cpp
op->work = [](context_base&, io_base* io) -> submit_result {
    auto* data = static_cast<iocp_io_data*>(io->extra.get());
    auto* rop  = static_cast<receive_operation*>(io);

    // ... 错误处理 ...

    ::std::get<2>(*rop) = static_cast<::std::size_t>(data->result);
    io->complete();
    return submit_result::ready;
};
```

因此，同步完成时上层 `co_await net::async_receive(...)` 得到的返回值是 `0`。

在 `http-server.cpp` 中，`make_client` 的主循环是：

```cpp
while (auto n = co_await timeout(scheduler, 3s,
                                 net::async_receive(stream, net::buffer(buffer)))) {
    ...
}
```

`n == 0` 被解释为客户端已关闭连接，于是 `make_client` 退出，`stream` 析构关闭 socket。而真正的客户端还在等待响应，所以看到的是 `10054` 连接重置。

## 为什么 accept/connect 没问题

`accept()` 和 `connect()` 的同步完成路径本来就写对了：

```cpp
if (ok) {
    raw->result = static_cast<int>(bytes);
    op->work(*this, op);     // ✅ 调用了 work
    return submit_result::ready;
}
```

所以问题只局限在 `receive()` 和 `send()` 的同步分支。

## 修复

把 `receive()` 和 `send()` 的同步成功路径改为先调用 `op->work()`：

```cpp
if (rc == 0) {
    data.result = static_cast<int>(bytes);
    op->work(*this, op);     // ✅ 由 work 设置 std::get<2>(*op)，再完成
    return submit_result::ready;
}
```

修改位置：

- `stdexec/include/netexec/__detail/iocp_context.hpp` 中 `receive()` 的同步分支。
- `stdexec/include/netexec/__detail/iocp_context.hpp` 中 `send()` 的同步分支。

这样无论是同步完成还是异步完成，都会走同一条 `work` lambda，操作状态中的字节数一致。

## 验证

修复后在 `NET_EXEC_USE_IOCP` 下重新编译并测试：

```powershell
cmake --build build_iocp --config Release --target netexec.http-server
```

测试命令：

```powershell
curl -s -i http://127.0.0.1:12345/ --max-time 5
```

结果：完整返回 200 响应，不再出现 `10054`。

同时验证：

- `netexec.simple_echo_server.exe`：echo 正常。
- `netexec.server.exe` + `netexec.client.exe`：连接通信正常。
- `netexec.http-warwick.exe`：页面可正常返回。

## 总结

方案 B 本身的设计没有问题。这次 bug 是方案 B 实现里两个同步完成分支少调用了一次 `work()`，导致操作状态未正确填充。核心教训是：

> 同步完成和异步完成必须使用同一个 `work` lambda 来填充操作状态，保持状态一致性。

## 相关文件

- `stdexec/include/netexec/__detail/iocp_context.hpp`
- `stdexec/examples/netexec/http-server.cpp`（仅删除 `throw` 后不可达的 `return 0;`）
