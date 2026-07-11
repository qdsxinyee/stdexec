# Windows IOCP 三种 socket 模式对比

这个目录里有三个最小可编译示例，展示了在 Windows IOCP 上处理 socket 的三种方式：

| 文件 | 模式 |
|------|------|
| `iocp_blocking.cpp` | **阻塞 socket + IOCP**（netexec 当前做法） |
| `iocp_nonblocking_inline.cpp` | **非阻塞 socket + 同步完成 inline 处理** |
| `iocp_skip_sync.cpp` | **非阻塞 socket + `SetFileCompletionNotificationModes`** |

三个都是监听 `12345` 端口的最小 echo server。编译方式见文末。

---

## 一、公共前提

三个示例都遵循同一个基本流程：

1. 初始化 Winsock。
2. 创建 IOCP：`CreateIoCompletionPort(...)`。
3. 创建监听 socket，绑定到 IOCP。
4. 用 `AcceptEx` 投递一个异步 accept。
5. 进入循环：`GetQueuedCompletionStatus(...)` 取完成包，根据类型处理。

区别只在于 **socket 是阻塞还是非阻塞**，以及 **同步完成时怎么办**。

---

## 二、方案 1：阻塞 socket + IOCP

对应文件：`iocp_blocking.cpp`

### 核心代码

```cpp
u_long mode = 0; // 0 == blocking
::ioctlsocket(s, FIONBIO, &mode);
```

监听 socket、accept 出来的 socket、connect 后的 socket，全部强制阻塞。

### 接收流程

```
App
 |
 |  WSARecv(blocking socket, &ol)
 v
+-----------------------------+
|  数据已到达？                |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  rc == 0       rc == SOCKET_ERROR
  bytes 返回      err == WSA_IO_PENDING
      |               |
      |               v
      |         Kernel 把操作挂到 IOCP
      |               |
      |               v
      |         数据到达
      |               |
      |               v
      |         GetQueuedCompletionStatus(&ol)
      |               |
      +-------+-------+
              |
              v
      处理 transferred 字节
              |
              v
      投递下一个 WSARecv
```

> 在这个实现里，即使 `rc == 0`（同步完成），也统一按 pending 处理，等 IOCP 完成包。严格说这会有“同步完成但 IOCP 不投递”的隐患，所以示例里强制阻塞，尽量让操作走 `WSA_IO_PENDING`。

### 发送流程

```
App
 |
 |  WSASend(blocking socket, &ol)
 v
+-----------------------------+
|  发送缓冲区足够？            |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  rc == 0       rc == SOCKET_ERROR
  bytes 发送      err == WSA_IO_PENDING
      |               |
      |               v
      |         Kernel 挂到 IOCP
      |               |
      |               v
      |         GetQueuedCompletionStatus(&ol)
      |               |
      +-------+-------+
              |
              v
      处理发送完成
              |
              v
      投递下一个 WSARecv
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 实现简单 | 直觉上和“IOCP 用非阻塞 socket”习惯相反 |
| 不会遇到 `WSAEWOULDBLOCK` | 每个 socket 创建时多一次 `ioctlsocket` |
| 状态机路径单一 | `AcceptEx`/`ConnectEx` 后还要再设一次阻塞 |

---

## 三、方案 2：非阻塞 socket + inline 完成

对应文件：`iocp_nonblocking_inline.cpp`

### 核心代码

```cpp
u_long mode = 1; // 1 == non-blocking
::ioctlsocket(s, FIONBIO, &mode);
```

`WSARecv`/`WSASend` 返回后，如果 `rc == 0`，**立即在调用栈里处理完成**。

### 接收流程

```
App
 |
 |  WSARecv(non-blocking socket, &ol)
 v
+-----------------------------+
|  rc == 0 ?                   |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  同步完成          err == WSA_IO_PENDING
      |               |
      v               v
  on_recv_inline    挂到 IOCP，等待...
      |               |
      v               v
  处理 bytes        GetQueuedCompletionStatus(&ol)
      |               |
      v               v
  投递下一操作      on_recv_complete
      |               |
      +-------+-------+
              |
              v
           继续循环
```

### 发送流程

```
App
 |
 |  WSASend(non-blocking socket, &ol)
 v
+-----------------------------+
|  rc == 0 ?                   |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  同步完成          err == WSA_IO_PENDING
      |               |
      v               v
  on_send_inline    挂到 IOCP，等待...
      |               |
      v               v
  处理发送完成      GetQueuedCompletionStatus(&ol)
      |               |
      v               v
  投递下一操作      on_send_complete
      |               |
      +-------+-------+
              |
              v
           继续循环
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 热路径可能少一次内核 IOCP 投递 | 同步完成会触发回调，可能递归很深 |
| 更符合 IOCP 常规认知 | `this` 可能被 inline complete 销毁，use-after-free 风险 |
| | 如果 `WSAEWOULDBLOCK` 出现，要额外重试逻辑 |

---

## 四、方案 3：非阻塞 socket + Skip Sync Completion

对应文件：`iocp_skip_sync.cpp`

### 核心代码

```cpp
u_long mode = 1;
::ioctlsocket(s, FIONBIO, &mode);

UCHAR flags = FILE_SKIP_COMPLETION_PORT_ON_SUCCESS
            | FILE_SKIP_SET_EVENT_ON_HANDLE;
::SetFileCompletionNotificationModes(reinterpret_cast<HANDLE>(s), flags);
```

这告诉 Windows：**同步完成的操作不要往 IOCP 端口投递完成包**，结果直接从 `WSARecv`/`WSASend` 返回值里拿。

### 接收流程

```
App
 |
 |  WSARecv(non-blocking + skip-sync socket, &ol)
 v
+-----------------------------+
|  rc == 0 ?                   |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  同步完成          err == WSA_IO_PENDING
      |               |
      | (NO IOCP packet)
      v               v
  on_recv_inline    Kernel 挂到 IOCP
      |               |
      v               v
  处理 bytes        数据到达
      |               |
      v               v
  投递下一操作      GetQueuedCompletionStatus(&ol)
                          |
                          v
                     on_recv_complete
```

### 发送流程

```
App
 |
 |  WSASend(non-blocking + skip-sync socket, &ol)
 v
+-----------------------------+
|  rc == 0 ?                   |
+-------------+---------------+
              |
      +-------+-------+
      |               |
     是              否
      |               |
      v               v
  同步完成          err == WSA_IO_PENDING
      |               |
      | (NO IOCP packet)
      v               v
  on_send_inline    Kernel 挂到 IOCP
      |               |
      v               v
  处理发送完成      GetQueuedCompletionStatus(&ol)
      |               |
      v               v
  投递下一操作      on_send_complete
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 非阻塞 socket，符合习惯 | 需要 Vista+ |
| 同步完成不投递 IOCP，避免 double completion | 代码更复杂 |
| 保留 IOCP 批量处理异步完成的能力 | 老系统需要 fallback |
| 没有 inline complete 导致的递归问题 | |

---

## 五、三种方案对比

| 特性 | 阻塞 + IOCP | 非阻塞 + inline | 非阻塞 + skip sync |
|------|------------|-----------------|-------------------|
| socket 模式 | 阻塞 | 非阻塞 | 非阻塞 |
| 同步完成时 | 假设走 pending，等 IOCP | 立即 inline 处理 | 立即 inline 处理 |
| 是否会有 IOCP 同步完成包 | 可能有（默认行为） | 可能有 | 明确没有 |
| 是否需要处理 `WSAEWOULDBLOCK` | 否 | 是 | 是 |
| 是否需要 `SetFileCompletionNotificationModes` | 否 | 否 | 是 |
| 递归/栈风险 | 低 | 高 | 低 |
| 实现复杂度 | 低 | 中 | 高 |
| 理论极限性能 | 好 | 热路径最好 | 最好（兼顾批量） |

---

## 六、编译运行

打开 **x64 Native Tools Command Prompt**，进入本目录：

```cmd
cd E:\code\cpp\library\stdexec\docs\kimidoc\iocptest
cl /EHsc /W3 iocp_blocking.cpp ws2_32.lib
cl /EHsc /W3 iocp_nonblocking_inline.cpp ws2_32.lib
cl /EHsc /W3 iocp_skip_sync.cpp ws2_32.lib
```

或者直接用目录里的 `build.bat`：

```cmd
build.bat
```

运行任一示例：

```cmd
iocp_blocking.exe
```

然后用任意 TCP 客户端连接 `127.0.0.1:12345` 发送数据，都会收到 echo。

---

## 七、和 netexec 的关系

- `stdexec/include/netexec/__detail/iocp_context.hpp` 当前采用的是 **方案 1（阻塞 socket）**。
- `net/include/beman/net/detail/iocp_context.hpp` 目前**没有任何一种**强制阻塞/同步完成的处理，基本属于“非阻塞但还没处理 inline”的状态。
- 如果要让 netexec 的 IOCP 后端改成非阻塞，**方案 3（skip sync）**通常是生产环境首选；**方案 2（inline）**更适合做最小概念验证。
