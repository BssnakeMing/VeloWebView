#pragma once

// One bridge implementation for CEF, WebView2 and the Android backends.
// Native transports carry strings only; application values use JSON explicitly.
namespace VeloWebView
{
inline constexpr const char* JavaScriptBridgeSource = R"VELOJS(
(function installVeloBridge(attempt) {
  'use strict';
  if (window !== window.top) return;
  if (window.VeloWebView && window.VeloWebView.__veloVersion === 1) return;
  var original = window.VeloWebView;
  var hasTransport = typeof window.__veloNativePostMessage === 'function' ||
    (window.chrome && window.chrome.webview) ||
    (window.ue && window.ue.velobridge && typeof window.ue.velobridge.postmessage === 'function') ||
    (original && typeof original.postMessage === 'function');
  if (!hasTransport) {
    // UE's Android UObject binding may appear after the document starts loading.
    if ((attempt || 0) < 200) setTimeout(function () { installVeloBridge((attempt || 0) + 1); }, 50);
    return;
  }
  var pending = new Map();
  var events = new EventTarget();
  var session = Date.now().toString(36) + '-' + Math.random().toString(36).slice(2);
  var sequence = 0;
  var prefix = '__VELO_RPC__';
  function serialize(value) {
    var json = JSON.stringify(value);
    return json === undefined ? 'null' : json;
  }
  function send(message) {
    // Count UTF-8 bytes, including escaped surrogate code units, on every backend.
    var bytes = 0;
    for (var index = 0; index < message.length; ++index) {
      var unit = message.charCodeAt(index);
      if (unit < 128) bytes += 1;
      else if (unit < 2048) bytes += 2;
      else if (unit >= 0xD800 && unit <= 0xDBFF && index + 1 < message.length &&
               message.charCodeAt(index + 1) >= 0xDC00 && message.charCodeAt(index + 1) <= 0xDFFF) { bytes += 4; ++index; }
      else bytes += 3;
      if (bytes > 1048552) throw new Error('VeloWebView message exceeds the transport size limit');
    }
    if (typeof window.__veloNativePostMessage === 'function') {
      window.__veloNativePostMessage(message);
    } else if (window.chrome && window.chrome.webview) {
      window.chrome.webview.postMessage(message);
    } else if (window.ue && window.ue.velobridge) {
      window.ue.velobridge.postmessage(message);
    } else if (original && typeof original.postMessage === 'function') {
      original.postMessage(message);
    } else {
      throw new Error('VeloWebView native transport is unavailable');
    }
  }
  function packet(value) { value.version = 1; value.session = session; send(prefix + JSON.stringify(value)); }
  function errorText(error) {
    try { return String(error && error.message !== undefined ? error.message : error); }
    catch (_) { return 'JavaScript error'; }
  }
  var api = {
    __veloVersion: 1,
    __session: session,
    isReady: true,
    onmessage: null,
    postMessage: function (value) { send(typeof value === 'string' ? value : serialize(value)); },
    addEventListener: events.addEventListener.bind(events),
    removeEventListener: events.removeEventListener.bind(events),
    request: function (method, params, timeoutMs) {
      return new Promise(function (resolve, reject) {
        if (!api.isReady) { reject(new Error('VeloWebView page was unloaded')); return; }
        if (typeof method !== 'string' || !method.trim()) { reject(new Error('A method name is required')); return; }
        if (pending.size >= 128) { reject(new Error('Too many pending VeloWebView requests')); return; }
        var id = session + ':' + (++sequence);
        var timeout = timeoutMs === undefined ? 30000 : Number(timeoutMs);
        if (!Number.isFinite(timeout) || timeout <= 0) { reject(new Error('Invalid request timeout')); return; }
        timeout = Math.min(timeout, 300000);
        var timer = setTimeout(function () {
          pending.delete(id);
          try { packet({ kind: 'cancel', id: id }); } catch (_) {}
          reject(new Error('VeloWebView request timed out: ' + method));
        }, timeout);
        pending.set(id, { resolve: resolve, reject: reject, timer: timer });
        try { packet({ kind: 'request', id: id, method: method, params: params === undefined ? null : params, timeoutMs: timeout }); }
        catch (error) { clearTimeout(timer); pending.delete(id); reject(error); }
      });
    },
    __receive: function (message) {
      if (!message || message.version !== 1 || message.session !== session || !api.isReady) return;
      if (message.kind === 'response') {
        var entry = pending.get(message.id);
        if (!entry) return;
        pending.delete(message.id); clearTimeout(entry.timer);
        if (message.success) entry.resolve(message.result);
        else entry.reject(new Error(message.error || 'Native request failed'));
      } else if (message.kind === 'message') {
        var event = new MessageEvent('message', { data: message.data });
        events.dispatchEvent(event);
        if (typeof api.onmessage === 'function') {
          try { api.onmessage(event); } catch (error) { setTimeout(function () { throw error; }, 0); }
        }
      }
    },
    __evaluate: function (id, source, expectedSession) {
      if (expectedSession !== session || !api.isReady) return;
      Promise.resolve().then(function () { return (0, eval)(source); }).then(function (value) {
        packet({ kind: 'result', id: id, success: true, result: JSON.parse(serialize(value)) });
      }).catch(function (error) {
        try { packet({ kind: 'result', id: id, success: false, error: errorText(error) }); } catch (_) {}
      });
    }
  };
  window.VeloWebView = api;
  window.addEventListener('pagehide', function () {
    api.isReady = false;
    try { packet({ kind: 'unloaded' }); } catch (_) {}
    pending.forEach(function (entry, id) {
      clearTimeout(entry.timer);
      try { packet({ kind: 'cancel', id: id }); } catch (_) {}
      entry.reject(new Error('VeloWebView page was unloaded'));
    });
    pending.clear();
  });
  window.addEventListener('pageshow', function () {
    if (!api.isReady) { api.isReady = true; packet({ kind: 'ready' }); }
  });
  packet({ kind: 'ready' });
  window.dispatchEvent(new Event('velowebviewready'));
})();
)VELOJS";
}
