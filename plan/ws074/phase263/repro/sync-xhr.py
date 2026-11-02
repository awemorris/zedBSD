#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Checks a synchronous XMLHttpRequest with a method, headers and a body against a local HTTP server.

  sync-xhr.py [--program PATH]

Starts a server on 127.0.0.1 (a free port) whose page sends a synchronous POST with a JSON body and a header,
then a synchronous PUT and a GET with a 404; the server echoes what it got.  browser --run loads the page and
prints PASS or FAIL lines; the script exits 0 when every line passed.
"""

import argparse
import http.server
import json
import os
import subprocess
import sys
import threading

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "../../../.."))

PAGE = r"""<!DOCTYPE html>
<html><body><script>
var results = [];
function check(name, ok) { results.push((ok ? 'PASS ' : 'FAIL ') + name); }
var x = new XMLHttpRequest(), events = [];
x.addEventListener('load', function () { events.push('load'); });
x.addEventListener('loadend', function () { events.push('loadend'); });
x.open('POST', '/verify?provider=test', false);
x.setRequestHeader('Content-Type', 'application/json');
x.setRequestHeader('X-Test', 'yes');
x.send(JSON.stringify({ 'bm-verify': 'abc', pow: 12345 }));
check('post status', x.status === 200);
check('post readyState', x.readyState === 4);
var d = {};
try { d = JSON.parse(x.responseText); } catch (e) {}
check('post method', d.method === 'POST');
check('post path', d.path === '/verify?provider=test');
check('post body', d.body === '{"bm-verify":"abc","pow":12345}');
check('post content-type', d.type === 'application/json');
check('post header', d.test === 'yes');
check('post response header', x.getResponseHeader('X-Echo') === 'done');
check('post events', events.join() === 'load,loadend');
var p = new XMLHttpRequest();
p.open('PUT', '/verify', false);
p.send('plain');
var e = {};
try { e = JSON.parse(p.responseText); } catch (err) {}
check('put method and body', e.method === 'PUT' && e.body === 'plain');
var n = new XMLHttpRequest();
n.open('GET', '/missing', false);
n.send();
check('get 404', n.status === 404);
console.log(results.join('\n'));
console.log(results.every(function (r) { return r.indexOf('PASS') === 0; }) ? 'ALL PASS' : 'SOME FAIL');
</script></body></html>
"""


class Handler(http.server.BaseHTTPRequestHandler):
    """Serves the page and echoes the requests to /verify."""

    def log_message(self, *args):
        pass

    def reply(self, status, body, kind):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("X-Echo", "done")
        self.end_headers()
        self.wfile.write(data)

    def echo(self):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length).decode("utf-8") if length else ""
        self.reply(200, json.dumps({"method": self.command, "path": self.path, "body": body,
                                    "type": self.headers.get("Content-Type"), "test": self.headers.get("X-Test")}),
                   "application/json")

    def do_GET(self):
        if self.path == "/":
            self.reply(200, PAGE, "text/html; charset=utf-8")
        else:
            self.reply(404, "not found", "text/plain")

    do_POST = echo
    do_PUT = echo


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", default=os.path.join(ROOT, "build/ws074-host/plain/browser"))
    options = parser.parse_args()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = "http://127.0.0.1:%d/" % server.server_address[1]
    result = subprocess.run([options.program, "--run", url], capture_output=True, text=True, timeout=60)
    server.shutdown()
    sys.stdout.write(result.stdout + result.stderr)
    return 0 if "ALL PASS" in result.stdout + result.stderr else 1


if __name__ == "__main__":
    sys.exit(main())
