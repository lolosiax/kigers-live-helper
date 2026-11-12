/*
 * WebView2 桥的浏览器侧门面。
 *
 * 只在 `window.chrome.webview` 存在时可用；浏览器开发模式下 available() 为 false，
 * wasm 侧会自动切到 MockMirrorEngine。
 */
globalThis.klive = {
  available: () => !!(globalThis.chrome && globalThis.chrome.webview),
  post: (message) => globalThis.chrome.webview.postMessage(message),
  listen: (callback) => globalThis.chrome.webview.addEventListener('message', (event) => {
    callback(typeof event.data === 'string' ? event.data : JSON.stringify(event.data));
  }),
};
