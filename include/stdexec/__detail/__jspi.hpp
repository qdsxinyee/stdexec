/*
 * Copyright (c) 2026 the stdexec.net contributors
 *
 * Licensed under the Apache License Version 2.0 with LLVM Exceptions
 * (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 *
 *   https://llvm.org/LICENSE.txt
 */
#pragma once

// Event-loop-driven wait for blocking primitives (Emscripten, opt-in).
//
// On the web the browser owns the event loop: a thread-level blocking wait
// (as sync_wait's run_loop does natively) starves rAF/timer callbacks and
// deadlocks. When the consumer opts in, the wait parks on a one-shot
// promise that the queue's push side resolves:
//
//   wait_for_item(): __jspi::park(handle)  -- EM_ASYNC_JS import; under
//     -sJSPI it is a Suspending import (whole wasm stack switched out until
//     the promise settles), under -sASYNCIFY it unwinds/rewinds. Either way
//     the browser gets a full turn and the stack resumes the moment the
//     queue's notify runs -- zero polling.
//   push(): __jspi::resolve(handle)  -- settles the parked promise (no-op
//     when nothing is parked on that handle).
//
// Handles are the waitable object's address, so multiple run_loops coexist.
//
// Build requirement: link with `-sJSPI` (or `-sASYNCIFY`) and define
// `STDEXEC_EMSCRIPTEN_JSPI=1` on the compile line. Without the opt-in
// define this header is empty and the native blocking path is
// byte-identical to before.

#if defined(__EMSCRIPTEN__) && defined(STDEXEC_EMSCRIPTEN_JSPI)

#include <cstdint>
#include <emscripten.h>
#include <emscripten/em_js.h>

namespace STDEXEC {
  namespace __jspi {

    // Resolve side: settle the parked promise for `handle`, if any. Safe to
    // call when nothing is parked (the JS map lookup misses).
    EM_JS(void, __stdexec_jspi_resolve, (std::uintptr_t handle), {
      const pending = (globalThis.__stdexecPending ??= new Map());
      const wake = pending.get(handle);
      if (wake) {
        pending.delete(handle);
        wake();
      }
    });

    inline void resolve(std::uintptr_t handle) noexcept {
      __stdexec_jspi_resolve(handle);
    }

    // Park side: one-shot promise, settled by resolve(handle). EM_ASYNC_JS
    // (file scope: the macro expands to an import declaration plus JS
    // metadata and cannot live inside a class). Under -sJSPI the import is
    // Suspending-wrapped automatically; under -sASYNCIFY it unwinds.
    EM_ASYNC_JS(void, __stdexec_jspi_park, (std::uintptr_t handle), {
      const pending = (globalThis.__stdexecPending ??= new Map());
      return new Promise((wake) => pending.set(handle, wake));
    });

    inline void park(std::uintptr_t handle) noexcept {
      __stdexec_jspi_park(handle);
    }

    template <class T>
    inline std::uintptr_t handle_of(T const *object) noexcept {
      return reinterpret_cast<std::uintptr_t>(object);
    }

  }  // namespace __jspi
}  // namespace STDEXEC

#endif  // defined(__EMSCRIPTEN__) && defined(STDEXEC_EMSCRIPTEN_JSPI)
