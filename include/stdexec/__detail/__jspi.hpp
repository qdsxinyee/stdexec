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

// Browser-friendly wait for blocking primitives (Emscripten, opt-in).
//
// On the web the browser owns the event loop: a thread-level blocking wait
// (as sync_wait's run_loop does natively) starves rAF/timer callbacks and
// deadlocks. When the consumer opts in, the wait yields with
// `emscripten_sleep(1)`:
//
//   - under `-sJSPI`, emscripten wires `emscripten_sleep` as a suspending
//     import -- the whole wasm stack is switched out and resumes after the
//     browser ran its queued work (JSPI is emscripten's ASYNCIFY == 2 mode;
//     the library side needs no extra imports or glue),
//   - under `-sASYNCIFY`, it unwinds/rewinds the stack instead.
//
// Either way the browser gets a turn and the caller re-checks its predicate
// when the stack resumes. Polling at event-loop granularity is the natural
// cadence for a UI loop.
//
// Build requirement: link with `-sJSPI` (or `-sASYNCIFY`) and define
// `STDEXEC_EMSCRIPTEN_JSPI=1` on the compile line. Without the opt-in
// define this header is empty and the native blocking path is
// byte-identical to before.

#if defined(__EMSCRIPTEN__) && defined(STDEXEC_EMSCRIPTEN_JSPI)

#include <emscripten.h>

namespace STDEXEC {
  namespace __jspi {

    // Yield one browser event-loop turn. Resumes after the browser has had
    // the chance to run queued work; the caller re-checks its predicate.
    inline void yield() noexcept { emscripten_sleep(1); }

  }  // namespace __jspi
}  // namespace STDEXEC

#endif  // defined(__EMSCRIPTEN__) && defined(STDEXEC_EMSCRIPTEN_JSPI)
