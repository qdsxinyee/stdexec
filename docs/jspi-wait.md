# JSPI-capable sync_wait (Emscripten, opt-in)

`sync_wait` joins by parking the calling thread's `run_loop` on its queue.
On the web the browser owns the event loop: blocking that thread starves
rAF/timer callbacks and deadlocks. This fork adds an opt-in path where the
`run_loop`'s wait yields with `emscripten_sleep(1)`:

- under `-sJSPI`, emscripten wires `emscripten_sleep` as a suspending import
  (JSPI is emscripten's ASYNCIFY == 2 mode) — the whole wasm stack is
  switched out, the browser runs its queued work, and the stack resumes when
  the sleep timer fires. The wait loop then re-checks its queue predicate;
  polling at event-loop granularity is the natural cadence for a UI loop.
- under `-sASYNCIFY` (mode 1), the same call unwinds/rewinds the stack.

## Enabling

Consumer-side only (stdexec itself stays an INTERFACE library):

```cmake
target_compile_definitions(<you> PUBLIC STDEXEC_EMSCRIPTEN_JSPI=1)  # compile-time opt-in
target_link_options(<you>    PUBLIC -sJSPI)                          # link-time mechanism
```

No JS glue is required: `emscripten_sleep` is an emscripten-provided import
and is wrapped automatically (`emscripten_sleep__async: 'auto'` in
`src/lib/libasync.js`; `instrumentWasmImports` matches it through the
`__asyncjs__*` default import pattern under ASYNCIFY == 2).

## Entry wiring for INVOKE_RUN=0 apps

If you build with `-sINVOKE_RUN=0` and drive `main` yourself, list `main` in
`JSPI_EXPORTS` so the export is promising-wrapped, then await it from JS
after the runtime is ready (see
`tests/emscripten/jspi-entry.js` + `jspi_smoke.cpp`):

```js
const ret = await Module.ccall('main', 'number', [], []);
```

Main's internal suspensions (sync_wait's park) suspend and resume across
this boundary; without the JSPI_EXPORTS listing you get
`SuspendError: trying to suspend without WebAssembly.promising`.

## Verified (Chrome stable, 2026-11)

End-to-end smoke (`tests/emscripten/jspi_smoke.cpp` + cdp driver): main
suspends inside sync_wait's park; a browser timer finishes the loop; the
stack resumes and sync_wait completes with `set_stopped`. Runtime markers
(PASS-JSPI in the DOM) present and reproducible.

Known behaviors:
- `emscripten_sleep(0)` does not reliably suspend in this emsdk — the wait
  yields with `sleep(1)`.
- A suspend attempt from a non-promising entry throws
  `SuspendError: ... WebAssembly.promising` (e.g. a `setTimeout` callback
  entering wasm directly); the promising entry (main via JSPI_EXPORTS/ccall)
  is the sanctioned path.

## Files

- `include/stdexec/__detail/__jspi.hpp` — the guarded yield.
- `include/stdexec/__detail/__atomic_intrusive_queue.hpp` — the only
  behavioral change: `wait_for_item()` yields under the opt-in define;
  without it, byte-identical to upstream.
- `tests/emscripten/` — smoke + entry driver.
