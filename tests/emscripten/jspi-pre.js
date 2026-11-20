// Surface early page errors on the title (visible via dump-dom/CDP).
window.addEventListener('error', (e) => {
  document.title += ' ERR: ' + String(e.error ?? e.message).slice(0, 120);
});
window.addEventListener('unhandledrejection', (e) => {
  document.title += ' REJ: ' + String(e.reason).slice(0, 120);
});
