# MSVC build failure in `test_let_error.cpp` when building `test.stdexec`

## Summary

When building the `test.stdexec` target on Windows with MSVC, a test case near line 101 of `test/stdexec/algos/adaptors/test_let_error.cpp` triggers a compile-time check failure in `__let.hpp`:

```
_FUNCTION_MUST_RETURN_A_VALID_SENDER_IN_THE_CURRENT_ENVIRONMENT_
_UNABLE_TO_COMPUTE_THE_SENDER_COMPLETION_SIGNATURES_
_WITH_SENDER_<void>
```

This failure is unrelated to the `let_async_scope` changes; it is a pre-existing MSVC compatibility issue in the `let_error` test.

## Reproduction

```bash
cd build
cmake --build . --target test.stdexec --config Release
```

## Detailed analysis

The failing test code is in `test/stdexec/algos/adaptors/test_let_error.cpp`:

```cpp
ex::sender auto snd = ex::just()
  | ex::then([]() -> std::string { throw std::logic_error{"error description"}; })
  | ex::let_error([](std::exception_ptr eptr) {
      STDEXEC_TRY {
        std::rethrow_exception(eptr);
      }
      STDEXEC_CATCH(std::exception const & e) {
        return ex::just(std::string{e.what()});  // returns a sender
      }
      STDEXEC_CATCH_FALLTHROUGH                  // no return
    });
```

`let_error` requires its function argument to **return a sender on all control paths**. In the code above, only the `STDEXEC_CATCH` branch returns `ex::just(...)`, while the `STDEXEC_CATCH_FALLTHROUGH` branch does not return a sender. On MSVC, the compiler deduces the lambda's return type as `void`, causing the `__well_formed_sender` concept check in `__let.hpp` to fail.

## Why this is unrelated to the current changes

- The failing file is `__let.hpp`, which implements `let_value` / `let_error` / `let_stopped`. It is **not** the `__let_async_scope.hpp` file modified in this task.
- `git status` shows `__let.hpp` is unchanged.
- The netexec targets (`netexec.let_async_scope_server` and `test.netexec`) build successfully, and all 41 test cases / 76 assertions in `test.netexec` pass.

## Impact

- Only affects the full MSVC build of `test.stdexec`.
- Does not affect `let_async_scope` functionality, the netexec example, or the netexec test suite.

## Possible fixes

1. Update the test case in `test_let_error.cpp` so that the `STDEXEC_CATCH_FALLTHROUGH` branch also returns a sender (e.g., `ex::just_error(std::exception_ptr{})` or a `std::terminate` wrapper).
2. Disable this test case for MSVC, or adjust the `STDEXEC_CATCH_FALLTHROUGH` macro expansion on MSVC so that it is recognized as non-returning.
3. Improve the diagnostic in `__let.hpp` to more clearly indicate that the lambda returns `void`.

## References

- `test/stdexec/algos/adaptors/test_let_error.cpp`, lines 99–117
- `include/stdexec/__detail/__let.hpp`, lines 500–520 (`__let_t::operator()` `__well_formed_sender` constraint)
