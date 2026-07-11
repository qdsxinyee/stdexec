# `initiate` 是否应重命名为 `async_initiate` 以符合 P3482R1 术语？

## 摘要

在当前 TAPS 实现中，`initiate` 的用法如下：

```cpp
auto exp{co_await (net::initiate(pre) | net::detail::into_expected)};
```

我想澄清一下预期的命名方式。[P3482R1](https://wg21.link/P3482R1)《基于 IETF TAPS 的 C++ 网络》在第 5 节（*Design discussion*）中提到：

> `async_connect()` is subsumed by the proposed `async_initiate()` operation.
> （`async_connect()` 被提议的 `async_initiate()` 操作所取代。）

因此，在 TAPS 层中主动打开连接的操作是否应该命名为 `async_initiate`？

```cpp
auto exp{co_await (net::async_initiate(pre) | net::detail::into_expected)};
```

## 问题

目前，`initiate` 似乎是基于 `async_connect` 实现的。请问计划是：

1. **重命名** `initiate` 为 `async_initiate`，以与 P3482R1 的术语保持一致；还是
2. **保留** `initiate` 作为高层封装，并单独引入 `async_initiate` 作为更底层的 TAPS CPO？

无论哪种方式，如果能说明预期的命名和分层设计，将会很有帮助。

## 备注

> tls 层 API 后续已从 `netexec::net` 迁移到 `netexec::net::tls`。如今等价的调用为：
> ```cpp
> auto exp{co_await (net::tls::async_initiate(pre, ctx) | net::detail::into_expected)};
> ```

## 参考资料

- [P3482R1](https://wg21.link/P3482R1) — 《基于 IETF TAPS 的 C++ 网络》，第 5 节：*Design discussion*
- [P2762](https://wg21.link/P2762) — 《Sender/Receiver Interface for Networking》
