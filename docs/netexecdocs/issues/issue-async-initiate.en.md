# Rename `initiate` to `async_initiate` to align with P3482R1 terminology

## Summary

In the current TAPS implementation, `initiate` is used like this:

```cpp
auto exp{co_await (net::initiate(pre) | net::detail::into_expected)};
```

I would like to clarify the intended naming. [P3482R1](https://wg21.link/P3482R1) "C++ Networking based on IETF TAPS" states in Section 5 (*Design discussion*):

> `async_connect()` is subsumed by the proposed `async_initiate()` operation.

Should the TAPS-level operation for actively opening a connection therefore be named `async_initiate`?

```cpp
auto exp{co_await (net::async_initiate(pre) | net::detail::into_expected)};
```

## Question

Currently, `initiate` appears to be implemented on top of `async_connect`. Is the plan to:

1. **Rename** `initiate` to `async_initiate` to align with P3482R1 terminology, or
2. **Keep** `initiate` as a higher-level wrapper and introduce `async_initiate` separately as the lower-level TAPS CPO?

Either way, it would be helpful to understand the intended naming and layering.

## Notes

> The tls-layer APIs have since moved from `netexec::net` into `netexec::net::tls`. The equivalent call today would be:
> ```cpp
> auto exp{co_await (net::tls::async_initiate(pre, ctx) | net::detail::into_expected)};
> ```

## References

- [P3482R1](https://wg21.link/P3482R1) — C++ Networking based on IETF TAPS, Section 5: *Design discussion*
- [P2762](https://wg21.link/P2762) — Sender/Receiver Interface for Networking
