// JSPI entry driver for the sync_wait smoke (INVOKE_RUN=0 build).
// `main` is listed in JSPI_EXPORTS, so the wasm export is promising-wrapped;
// ccall drives it and any suspension inside main suspends this JS await.
(async () => {
  try {
    // Let the CDP driver attach before main starts.
    await new Promise((r) => setTimeout(r, 300));
    const ret = await Module.ccall('main', 'number', [], []);
    document.title += ' RET:' + ret;
  } catch (e) {
    document.title += ' START-ERR: ' + String(e).slice(0, 120);
  }
})();
