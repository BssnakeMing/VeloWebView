"""End-to-end JavaScript bridge check using an isolated BrowserHost process."""
import argparse
import ctypes
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import struct
import threading
import time

from input_smoke import Host
from pathlib import Path

PREFIX = "__VELO_RPC__"
HTML = r"""<!doctype html><meta charset="utf-8"><title>Velo JS bridge smoke</title>
<script>
(function () {
  var bridge = window.VeloWebView;
  bridge.addEventListener('message', function (event) {
    var data = event.data;
    if (data && data.kind === 'native-object') bridge.postMessage('native-object:' + data.value);
  });
  bridge.onmessage = function (event) {
    if (event.data === 'native-message') bridge.postMessage('native-message:ok');
  };
  bridge.postMessage('plain:中文:🙂');
  bridge.postMessage('page-session:' + bridge.__session);
  console.log('bridge-console');
  bridge.request('sum', {a: 19, b: 23}, 5000).then(function (value) {
    bridge.postMessage('request:sum:' + value);
  }).catch(function (error) { bridge.postMessage('request:error:' + error.message); });
  bridge.request('reject', null, 5000).then(function () {
    bridge.postMessage('request:unexpected-success');
  }).catch(function (error) { bridge.postMessage('request:rejected:' + error.message); });
})();
</script>"""


def quoted_script(packet, session):
    packet = dict(packet, session=session)
    payload = json.dumps(packet, ensure_ascii=False, separators=(",", ":"))
    literal = json.dumps(payload, ensure_ascii=False)
    return "window.VeloWebView.__receive(JSON.parse(" + literal + "));"


def wait_event(host, predicate, timeout=10):
    deadline = time.monotonic() + timeout
    if not hasattr(host, "events"):
        host.events = []
    while time.monotonic() < deadline:
        for index, event in enumerate(host.events):
            if predicate(event):
                return host.events.pop(index)
        event = host.receive(timeout=min(0.25, deadline - time.monotonic()))
        if event is not None:
            host.events.append(event)
    raise AssertionError("timed out waiting for BrowserHost JavaScript event")


def read_event(host, timeout):
    deadline = time.monotonic() + timeout
    if not hasattr(host, "incoming"):
        host.incoming = bytearray()
    while time.monotonic() < deadline:
        if len(host.incoming) >= 24:
            magic, version, message_type, size, instance, sequence = struct.unpack_from(
                "<IHHIIQ", host.incoming)
            if magic != 0x56454C4F or version != 1 or size < 24:
                raise AssertionError("invalid BrowserHost event header")
            if len(host.incoming) >= size:
                payload = bytes(host.incoming[24:size]).decode("utf-8")
                del host.incoming[:size]
                return message_type, payload

        available = ctypes.wintypes.DWORD()
        if not host.kernel.PeekNamedPipe(host.pipe, None, 0, None, ctypes.byref(available), None):
            raise OSError(ctypes.get_last_error(), "pipe read")
        if available.value:
            buffer = ctypes.create_string_buffer(min(available.value, 256 * 1024))
            read = ctypes.wintypes.DWORD()
            if not host.kernel.ReadFile(host.pipe, buffer, len(buffer), ctypes.byref(read), None):
                raise OSError(ctypes.get_last_error(), "pipe read")
            host.incoming.extend(buffer.raw[:read.value])
        else:
            time.sleep(0.01)
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True, type=lambda value: Path(value).resolve())
    args = parser.parse_args()

    class PageHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            body = HTML.encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *_):
            pass

    server = ThreadingHTTPServer(("127.0.0.1", 0), PageHandler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    host = Host(args.host, url=url)
    host.kernel.PeekNamedPipe.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_void_p,
                                          ctypes.wintypes.DWORD, ctypes.c_void_p,
                                          ctypes.POINTER(ctypes.wintypes.DWORD), ctypes.c_void_p]
    host.kernel.ReadFile.argtypes = [ctypes.wintypes.HANDLE, ctypes.c_void_p,
                                     ctypes.wintypes.DWORD, ctypes.POINTER(ctypes.wintypes.DWORD),
                                     ctypes.c_void_p]
    host.receive = lambda timeout: read_event(host, timeout)
    try:
        host.connect()
        wait_event(host, lambda event: event[0] == 31)
        ready = wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                           json.loads(event[1][len(PREFIX):]).get("kind") == "ready")
        session = json.loads(ready[1][len(PREFIX):])["session"]

        plain = wait_event(host, lambda event: event[0] == 36 and event[1] == "plain:中文:🙂")
        console = wait_event(host, lambda event: event[0] == 35 and
                             json.loads(event[1]).get("message") == "bridge-console")
        assert json.loads(console[1])["source"].startswith(url)
        print("PASS: JS Unicode messages and console events -> BrowserHost pipe", flush=True)

        requests = {}
        for _ in range(2):
            event = wait_event(host, lambda value: value[0] == 36 and value[1].startswith(PREFIX) and
                               json.loads(value[1][len(PREFIX):]).get("kind") == "request")
            packet = json.loads(event[1][len(PREFIX):])
            assert packet["version"] == 1 and packet["kind"] == "request"
            requests[packet["method"]] = packet["id"]
        assert set(requests) == {"sum", "reject"}

        host.send(15, quoted_script({
            "version": 1,
            "kind": "response",
            "id": requests["sum"],
            "success": True,
            "result": 42,
        }, session).encode("utf-8"))
        host.send(15, quoted_script({
            "version": 1,
            "kind": "response",
            "id": requests["reject"],
            "success": False,
            "error": "native rejection",
        }, session).encode("utf-8"))
        wait_event(host, lambda event: event[0] == 36 and event[1] == "request:sum:42")
        wait_event(host, lambda event: event[0] == 36 and event[1] == "request:rejected:native rejection")
        print("PASS: JavaScript requests resolve and reject from a native response", flush=True)

        command = "window.VeloWebView.__evaluate('smoke-result','Promise.resolve({ok:true,value:42})',%s);" % json.dumps(session)
        host.send(15, command.encode("utf-8"))
        result = wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                            json.loads(event[1][len(PREFIX):]).get("kind") == "result" and
                            json.loads(event[1][len(PREFIX):]).get("id") == "smoke-result")
        result_packet = json.loads(result[1][len(PREFIX):])
        assert result_packet["kind"] == "result" and result_packet["id"] == "smoke-result"
        assert result_packet["success"] and result_packet["result"] == {"ok": True, "value": 42}
        print("PASS: UE-style JavaScript evaluation returns a Promise result", flush=True)

        source = "Promise.reject(new Error('evaluation rejected'))"
        command = "window.VeloWebView.__evaluate('smoke-error',%s,%s);" % (json.dumps(source), json.dumps(session))
        host.send(15, command.encode("utf-8"))
        failed = wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                            json.loads(event[1][len(PREFIX):]).get("id") == "smoke-error")
        failure = json.loads(failed[1][len(PREFIX):])
        assert failure["success"] is False and failure["error"] == "evaluation rejected"

        host.send(15, b"window.VeloWebView.request('timeout',null,100).catch(function(e){window.VeloWebView.postMessage('timeout:ok')});")
        pending = wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                             json.loads(event[1][len(PREFIX):]).get("method") == "timeout")
        pending_id = json.loads(pending[1][len(PREFIX):])["id"]
        wait_event(host, lambda event: event[0] == 36 and event[1] == "timeout:ok")
        wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                   json.loads(event[1][len(PREFIX):]).get("kind") == "cancel" and
                   json.loads(event[1][len(PREFIX):]).get("id") == pending_id)
        print("PASS: rejected evaluation and page request timeout/cancellation", flush=True)

        host.send(15, quoted_script({"version": 1, "kind": "message", "data": "native-message"}, session).encode("utf-8"))
        host.send(15, quoted_script({
            "version": 1,
            "kind": "message",
            "data": {"kind": "native-object", "value": 42},
        }, session).encode("utf-8"))
        wait_event(host, lambda event: event[0] == 36 and event[1] == "native-message:ok")
        wait_event(host, lambda event: event[0] == 36 and event[1] == "native-object:42")
        print("PASS: native-to-JS string and JSON event.data messages", flush=True)

        host.send(16, b"0")
        time.sleep(0.25)
        host.send(5, url.encode("ascii"))
        wait_event(host, lambda event: event[0] == 31)
        try:
            unexpected = wait_event(host, lambda event: event[0] == 36 and
                                    event[1] == "plain:中文:🙂", timeout=1)
        except AssertionError:
            unexpected = None
        assert unexpected is None, "page JavaScript ran after it was disabled"
        host.send(16, b"1")
        time.sleep(0.25)
        host.send(5, url.encode("ascii"))
        wait_event(host, lambda event: event[0] == 31)
        wait_event(host, lambda event: event[0] == 36 and event[1] == "plain:中文:🙂")
        print("PASS: CEF JavaScript can be disabled and re-enabled at runtime", flush=True)
        wait_event(host, lambda event: event[0] == 36 and event[1] == "page-session:" + session)
        next_session_event = wait_event(host, lambda event: event[0] == 36 and event[1].startswith("page-session:") and
                                        event[1] != "page-session:" + session)
        next_session = next_session_event[1].split(":", 1)[1]
        host.send(15, quoted_script({"version": 1, "kind": "message", "data": "native-message"}, session).encode("utf-8"))
        host.send(15, ("window.VeloWebView.__evaluate('stale-eval',\"window.VeloWebView.postMessage('stale-evaluated')\",%s);" %
                       json.dumps(session)).encode("utf-8"))
        host.send(15, ("window.VeloWebView.__evaluate('current-eval','42',%s);" % json.dumps(next_session)).encode("utf-8"))
        current = wait_event(host, lambda event: event[0] == 36 and event[1].startswith(PREFIX) and
                            json.loads(event[1][len(PREFIX):]).get("id") == "current-eval")
        assert json.loads(current[1][len(PREFIX):])["result"] == 42
        assert not any(event[0] == 36 and event[1] in ("native-message:ok", "stale-evaluated") for event in host.events)
        print("PASS: navigation ignores previous-document messages and evaluation", flush=True)
    finally:
        host.close()
        server.shutdown()


if __name__ == "__main__":
    main()
