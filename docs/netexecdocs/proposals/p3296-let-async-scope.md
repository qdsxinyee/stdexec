# let_async_scope（P3296R6）实现说明

本文档记录本仓库内 `stdexec::let_async_scope`（[P3296R6](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2026/p3296r6.html)）的实现状态与设计决策。它原本写在 plan.md 里，因题域属于 stdexec 内核而非 netexec 网络层，独立成文；netexec 的示例与并发模型依赖该算法，故文档保留在 netexecdocs 下。

实现代码：`include/stdexec/__detail/__let_async_scope.hpp`、`include/stdexec/__detail/__spawn.hpp`；测试：`test/netexec/test_let_async_scope.cpp`。

## 函数异常处理的两种方案（当前采用方案 A）

P3296R6 要求：如果传给 `let_async_scope` 的函数抛异常，需要立即对 scope 调用 `request_stop()`，等 scope join 完成后以 `set_error(std::exception_ptr)` 完成。

难点：函数正常时返回 sender 类型 `S`，异常时要返回 `just_error(exception_ptr)` sender，两者类型不同。`let_value` 要求函数返回类型固定，不能简单地在 lambda 里 `try/catch` 后返回另一种 sender。

- **方案 A（已实现）**：写一个小型的 `__fn_or_error_sender`，内部用 `std::variant` 存正常 sender 或 `just_error` sender，`connect` 时按 index 构造对应的 opstate（用 `STDEXEC::__variant` 管理 immovable opstate）。改动集中在 `__let_async_scope.hpp`。
- **方案 B（备选）**：不用 `let_value`，按 R6 的 `let-async-scope-bind` 重写第二阶段的连接逻辑。更贴近 wording，但代码量更大，相当于把 `let_value` 的一部分逻辑重新实现一遍。

选择方案 A 的原因：侵入性最小，不需要重新实现 `let_value` 的参数存储和 opstate variant 逻辑。

## bind 时序修复：join 不再先于 spawn 启动（已完成）

原 bind 结构为 `when_all(let_value(sndr, fn), state->join())`：join 与 fn 并发启动，而 fn 只能在前驱完成后才被调用，因此 join 启动时 scope 计数必然为零，`counting_scope` 会立即 closed 并完成 join；之后 fn 里 `spawn` 的工作 `try_associate` 失败，被静默丢弃，且 `when_all` 只等 fn 的返回 sender，导致提前完成。

同步前驱下 fn 在 join 子操作启动前内联执行完毕，spawn 抢在 scope 关闭前注册成功，所以旧测试从未暴露该问题；只有异步完成的前驱（如 `schedule(io_scheduler)`）才会触发。

修复（提交 `bddbb67b`）把 bind 改为：eager apply fn → 立即 `try_associate` 取得 nest 关联（计数 0→1）→ `token.wrap` 包装返回 sender → `when_all(nest_sender, join)`。关联在 join 启动前持有、在 fn 的 sender 完成后释放，与 R6 `let-async-scope-bind` 的 `nest(apply(fn, args), scope)` 语义对齐。回归测试为 `test_let_async_scope.cpp` 的 "waits for work spawned on an io scheduler"（异步前驱 + fn 内 spawn + 验证工作真正执行），Windows IOCP 与 Linux poll 后端下均通过。

## distinct spawn overload（待办 / 讨论中）

> **状态定位**：bind 时序修复完成后，let_async_scope 对 netexec 的功能需求已经完整；本节描述的独立 `spawn` 重载纯粹是 wording 合规问题，仅在计划向上游提交时才需要实施。

### R6 为什么要求独立重载

P3296R6 的 wording 明确要求：

> `scope-token-type` shall be a unique type, such that invoking `spawn(snd, token, env)` or `spawn(snd, token)` where `token` is an instance of `scope-token-type` invokes a **distinct overload of `spawn`**.

背后的原因是 `let_async_scope` 和普通 `counting_scope` 对 `spawn` 的语义要求不同：

| 场景 | 普通 `counting_scope` | `let_async_scope` |
|------|----------------------|-------------------|
| 被 spawn 的 sender 能否失败 | **不能**。`spawn` 的 `requires` 子句要求 `__never_sends<set_error_t>`，否则走 diagnostic 重载报编译错误。 | **可以**。scope 有明确的错误列表，错误被记录后统一在 join 完成后传播。 |
| 错误如何处理 | 不允许出现错误。 | 通过 `token.wrap()` 里的 `upon_error` 捕获，存入 `state.error`，并 `request_stop()`。 |
| 编译期检查内容 | sender 不能有任何 `set_error_t` 完成。 | sender 的 `set_error_t(E)` 中的 `E` 必须在 scope 的错误列表 `Errors...` 里。 |

如果共用同一个 `spawn` 重载，就无法同时满足这两种语义：要么放宽普通 `counting_scope` 的检查（允许失败），要么对 `let_async_scope` 做错误的检查（禁止失败）。所以 R6 要求 `let_async_scope` 的 token 类型是唯一的，让重载决议把它导到一个专门的 `spawn` 函数。

### 完整实现 R6 需要修改的部分

严格按 wording 实现独立重载，需要改 `include/stdexec/__detail/__spawn.hpp`：

1. **定义识别 concept**：
   ```cpp
   template <class _Token>
   concept __let_async_scope_token = scope_token<_Token> && requires {
     typename _Token::__scope_env_t;
     { _Token::__any_error_allowed_v } -> std::same_as<bool>;
   };
   ```

2. **新增独立 3 参重载**：在 `spawn_t` 里增加一个用 `__let_async_scope_token` 约束的 `operator()`，在里面做错误兼容性检查，然后执行 spawn 逻辑。

3. **约束现有重载**：把现有的 3 参重载加上 `!__let_async_scope_token<_Token>`，避免两个重载同时匹配：
   ```cpp
   template <sender _Sender, scope_token _Token, class _Env>
     requires (!__let_async_scope_token<_Token>)
           && __never_sends<set_error_t, _spawn_sndr_t<_Sender, _Token, _Env>, _Env>
   void operator()(_Sender&& __sndr, _Token __tkn, _Env&& __env) const
   ```

4. **抽公共逻辑**：两个重载后面的 `wrap` → `write_env` → 分配 opstate → `try_associate` → `start` 完全一样，抽成一个私有静态 helper（如 `__spawn_impl`），避免代码重复。

5. **处理 2 参重载**：`spawn(snd, token)` 目前委托给 3 参版本。加了独立重载后，2 参版本会自然委托到正确的 3 参重载，不需要额外修改。

### 当前的做法

我们目前**没有**改成独立重载，而是在现有 3 参重载的函数体里用 `if constexpr` 检测：

```cpp
template <sender _Sender, scope_token _Token, class _Env>
  requires __never_sends<set_error_t, _spawn_sndr_t<_Sender, _Token, _Env>, _Env>
void operator()(_Sender&& __sndr, _Token __tkn, _Env&& __env) const
{
  if constexpr (requires {
                  typename _Token::__scope_env_t;
                  _Token::__any_error_allowed_v;
                })
  {
    if constexpr (!_Token::__any_error_allowed_v)
    {
      using __scope_env_t = typename _Token::__scope_env_t;
      using __errs_t      = __error_types_of_t<_Sender, __scope_env_t>;
      __check_scope_errors<_Token, __errs_t>();
    }
  }
  // ... 原来的 spawn 逻辑
}
```

`__token` 在 `__let_async_scope.hpp` 里暴露了三个 hook（`__scope_env_t`、`__error_allowed_v`、`__any_error_allowed_v`），`spawn` 通过它们判断是否是 `let_async_scope` token，是则做错误兼容性检查。

### 为什么现在没有完整采取 R6 的做法

1. **功能等价**：`if constexpr` 方案在编译期同样能拒绝不兼容的错误类型，`static_assert` 信息明确，测试（`test_let_async_scope_fail1`）已验证。
2. **改动最小**：独立重载需要动 `spawn_t` 的重载结构、加 concept、抽公共逻辑，涉及面广。`if constexpr` 只在一个函数体里加几行。
3. **重载决议复杂**：`let_async_scope` token 的 `wrap()` 会把错误用 `upon_error` 吃掉，所以现有重载的 `__never_sends` 检查对它也能通过。如果不仔细处理约束，两个重载可能同时匹配，产生歧义。
4. **诊断信息已经足够**：`static_assert` 直接指向用户代码里的 `spawn` 调用，错误信息清晰。改成独立重载后，诊断变成约束失败，某些编译器下错误栈反而更长。

如果未来要提交到上游，建议按上面“完整实现 R6 需要修改的部分”改成独立重载，以严格符合 wording；否则当前实现已经满足功能需求。
