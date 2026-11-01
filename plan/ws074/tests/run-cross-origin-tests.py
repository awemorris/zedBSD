#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Check an iframe of another origin and the messages between the Windows.

  run-cross-origin-tests.py [--program build/ws074-host/plain/browser]

A loopback server answers on one port for two origins (http://127.0.0.1:PORT and
http://localhost:PORT).  The parent page (on 127.0.0.1) embeds a child page of the other
origin and talks with it as a widget and its page do (a parent's apps panel kept hidden until
its frame says it has drawn): the child posts its parent a message with a MessagePort it
transfers, sends events on its own port of that channel, and the parent shows the frame when
the second event comes.  Each check prints "ok NAME" or "FAIL NAME" through the console of
browser --run --async; the last line is "cross-origin-tests: N/M".  Also checked:
HTMLScriptElement.src (a <script> receiver, not only <img>), the reflected iframe attributes
(name, frameBorder, scrolling) and frameborder="0" removing the iframe's border.
"""
import argparse
import importlib.util
import http.server
from pathlib import Path
import subprocess
import tempfile
import threading
from urllib.parse import urlsplit

SPEC = importlib.util.spec_from_file_location(
    "acid_runner", Path(__file__).with_name("run-acid-tests.py")
)
ACID = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ACID)

# The checks the pages print, in no particular order.
CHECKS = [
    "script-src", "iframe-reflect", "frameborder-style",
    "content-document-null", "content-window-object", "content-window-same", "content-window-no-document",
    "message-origin", "message-source", "message-ports", "port-start-event", "port-render-event",
    "port-reply", "wrong-target-dropped", "frame-shown",
    "child-parent-no-document", "child-frame-element-null", "child-top-is-parent",
    "child-got-ping", "child-ping-origin", "child-ping-source",
]

PARENT = """<!doctype html><html><body>
<div id=panel><iframe id=f style="width:200px;height:100px;visibility:hidden"></iframe></div>
<script>
function check(name, value) { console.log((value ? 'ok ' : 'FAIL ') + name); }
function blocked(f) { try { f(); return false; } catch (e) { return e.name === 'SecurityError'; } }
var child = 'http://localhost:' + location.port;
var s = document.createElement('script');
s.src = '/lib.js';
check('script-src', s.src === location.origin + '/lib.js');
var f = document.getElementById('f');
f.name = 'app'; f.frameBorder = 0; f.scrolling = 'no';
check('iframe-reflect', f.getAttribute('name') === 'app' && f.getAttribute('frameborder') === '0' && f.getAttribute('scrolling') === 'no' && f.name === 'app');
check('frameborder-style', getComputedStyle(f).borderTopStyle === 'none');
var events = 0, wrong = false;
window.addEventListener('message', function (e) {
  if (e.data === 'wrong') { wrong = true; return; }
  if (e.data !== 'hello') return;
  check('message-origin', e.origin === child);
  check('message-source', e.source === f.contentWindow);
  check('message-ports', e.ports.length === 1);
  var port = e.ports[0];
  port.onmessage = function (m) {
    events++;
    if (m.data.event === 'start') check('port-start-event', events === 1);
    if (m.data.event === 'render') {
      check('port-render-event', events === 2);
      f.style.visibility = 'visible';
      check('frame-shown', getComputedStyle(f).visibility === 'visible');
      m.ports[0].postMessage('reply');
    }
  };
});
f.addEventListener('load', function () {
  check('content-document-null', f.contentDocument === null);
  var w = f.contentWindow;
  check('content-window-object', w !== null && typeof w.postMessage === 'function');
  check('content-window-same', w === f.contentWindow && w.window === w && w.self === w);
  check('content-window-no-document', blocked(function () { return w.document; }));
  w.postMessage('ping', child);
  setTimeout(function () { check('wrong-target-dropped', !wrong); }, 200);
});
f.src = child + '/child.html';
</script></body></html>
"""

CHILD = """<!doctype html><html><body><p>child</p>
<script>
function check(name, value) { console.log((value ? 'ok ' : 'FAIL ') + name); }
function blocked(f) { try { f(); return false; } catch (e) { return e.name === 'SecurityError'; } }
var parentOrigin = 'http://127.0.0.1:' + location.port;
check('child-parent-no-document', blocked(function () { return parent.document; }));
check('child-frame-element-null', window.frameElement === null);
check('child-top-is-parent', top === parent);
window.addEventListener('message', function (e) {
  if (e.data !== 'ping') return;
  check('child-got-ping', true);
  check('child-ping-origin', e.origin === parentOrigin);
  check('child-ping-source', e.source === parent);
});
parent.postMessage('wrong', 'http://example.com');
var channel = new MessageChannel();
parent.postMessage('hello', parentOrigin, [channel.port2]);
var reply = new MessageChannel();
reply.port1.onmessage = function (m) { check('port-reply', m.data === 'reply'); };
channel.port1.postMessage({ event: 'start' });
document.addEventListener('DOMContentLoaded', function () {
  channel.port1.postMessage({ event: 'render' }, [reply.port2]);
});
</script></body></html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    """Answers the two pages for whichever origin the request named."""

    def log_message(self, _format, *args):
        pass

    def do_GET(self):
        name = urlsplit(self.path).path
        pages = {"/parent.html": PARENT, "/child.html": CHILD}
        body = pages.get(name, "").encode("utf-8")
        self.send_response(200 if name in pages else 404)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", type=Path,
                        default=ACID.ROOT / "build/ws074-host/plain/browser")
    args = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        with tempfile.TemporaryDirectory(prefix="ws074-cross-origin-") as name:
            command, environment = ACID.command_base(
                args.program, ACID.fonts(), 400, 300, Path(name), "--run"
            )
            command.append("http://127.0.0.1:%d/parent.html" % server.server_port)
            result = subprocess.run(command, env=environment, capture_output=True,
                                    text=True, timeout=60)
    finally:
        server.shutdown()
    lines = result.stdout.splitlines()
    passed = 0
    for check in CHECKS:
        if ("ok " + check) in lines and ("FAIL " + check) not in lines:
            passed += 1
        else:
            print("FAIL", check)
    for line in lines:
        if line.startswith("FAIL ") or "Uncaught" in line:
            print(line)
    print("cross-origin-tests: %d/%d" % (passed, len(CHECKS)))
    return 0 if passed == len(CHECKS) and result.returncode == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
