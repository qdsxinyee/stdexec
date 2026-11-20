// JSPI sync_wait smoke: sync_wait's run_loop parks the wasm stack on the
// JSPI import; a browser timer then finish()es the loop -- the only way the
// parked wait_for_item() can return is the park/resolve pair this change
// adds. Without JSPI the program hangs (which is the negative result).
//
// The suspension requires main to be on a WebAssembly.promising chain:
// emscripten's default ASYNCIFY_EXPORTS already includes main under -sJSPI,
// so the app-side needs no entry glue. IMPORTANT: never defer the
// sync_wait into an emscripten_async_call/setTimeout callback -- a timer
// callback enters wasm on a non-promising stack and the suspend throws
// SuspendError.
//
// Build (from emsdk):
//   emcc -std=c++23 -sJSPI -sSINGLE_FILE -DSTDEXEC_EMSCRIPTEN_JSPI=1
//        -I <stdexec>/include jspi_smoke.cpp -o jspi_smoke.html
// Verify (chrome --headless=new + CDP driver; title carries the trail:
//   "... M2-main M3-parking M4-resumed -PASS" and body ends PASS-JSPI):
//   node cdp_check.mjs <url> 6000

#include <stdexec/execution.hpp>

#include <emscripten.h>
#include <cstdio>
#include <string>

namespace {

void mark(const char* m) {
    emscripten_run_script(
        (std::string("document.title += '") + m + "';").c_str());
}

}  // namespace

int main() {
    mark(" M2-main");
    emscripten_sleep(50);
    mark(" M2c-slept50");
    stdexec::run_loop loop;
    bool scheduled = false;

    // 100ms after main suspends, finish() the loop from a fresh browser
    // callback stack -- this resolves the parked wait_for_item().
    emscripten_async_call(
        [](void* p) {
            mark(" FINISH-FIRED");
            static_cast<stdexec::run_loop*>(p)->finish();
        }, &loop,
        0);

    // The schedule op pushes immediately and executes inline (before the
    // park); the loop then parks on the empty queue until finish().
    mark(" M3-parking");
    auto opt = stdexec::sync_wait(
        stdexec::schedule(loop.get_scheduler()) | stdexec::then([&] {
            scheduled = true;
            mark(" M3b-task");
        }));

    // finish() completes the loop with set_stopped: disengaged optional.
    bool stopped = !opt.has_value();
    mark(stopped ? " M4-resumed" : " M4-completed-inline");
    std::printf("scheduled=%d stopped=%d\n", scheduled ? 1 : 0,
                stopped ? 1 : 0);
    if (stopped) {
        std::printf("PASS: parked under JSPI and resumed via finish\n");
        emscripten_run_script(
            "document.body.textContent += 'PASS-JSPI'; "
            "document.title += '-PASS'");
        return 0;
    }
    emscripten_run_script("document.title += '-FAIL'");
    return 1;
}
