#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Runs the WPT subset of ws345's cross-origin frames and window postMessage with the headless browser.

  wpt-cross-origin.py [--program BROWSER] [--out DIR] [--port PORT] [--jobs N] [--threads] [TEST...]

The subset (2026-11-14 Q1's decision): html/browsers/origin/cross-origin-objects/*.html, webmessaging/*.htm(l)
and webmessaging/without-ports/*.html of the pinned suite build/ws074-suites/wpt (plan/ws074/tests/fetch-suites.sh
wpt).  The suite's own server (wpt serve) runs on PORT and the next ports (the http, https, ws ports of its
config), with a testharnessreport.js of this runner's that prints the results on the console.  The WPT host names
(web-platform.test and its subdomains, which the cross-origin tests use as other origins) are resolved to 127.0.0.1
by a shim preloaded into the server and the browser (wpt-hosts.c), so the host's /etc/hosts is not changed.  Each
test runs as browser --run --async --settle-ms=5000 (with --threads, browser --threads ... for the view of its own
threads, once the browser has that mode).  One line per test (its harness status and its subtests passed of all,
or NO-RESULT), then the totals; the lines are also written to OUT/results.txt.  Last line:
wpt-cross-origin: N files, M with results, P/T subtests passed.
"""

import argparse
import concurrent.futures
import json
import os
import pathlib
import signal
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[3]
SUITE = ROOT / "build/ws074-suites/wpt"
PREFIX = "WS345_WPT_COMPLETE "
HARNESS = {0: "OK", 1: "ERROR", 2: "TIMEOUT", 3: "PRECONDITION_FAILED"}
REPORT = """(function () {
    function test_result(test) {
        return {name: String(test.name), status: test.status,
                message: test.message ? String(test.message) : ""};
    }
    add_completion_callback(function (tests, harness_status) {
        var results = [];
        for (var i = 0; i < tests.length; i++) {
            results.push(test_result(tests[i]));
        }
        console.log("WS345_WPT_COMPLETE " + JSON.stringify({
            tests: results,
            status: harness_status.status,
            message: harness_status.message ? String(harness_status.message) : ""
        }));
    });
}());
"""


def subset():
    """The tests of the subset, as paths in the suite."""
    tests = sorted(SUITE.glob("html/browsers/origin/cross-origin-objects/*.html"))
    tests += sorted(p for p in SUITE.glob("webmessaging/*") if p.suffix in (".htm", ".html"))
    tests += sorted(SUITE.glob("webmessaging/without-ports/*.html"))
    return [str(p.relative_to(SUITE)) for p in tests]


def run_one(args, shim, test):
    """Runs one test; returns (test, line)."""
    fonts = ["--font=userland/desktop/fonts/Mahora-Regular.ttf",
             "--mono-font=userland/desktop/fonts/JetBrainsMono-Regular.ttf",
             "--fallback-font=userland/desktop/fonts/DroidSansFallbackFull.ttf"]
    command = [str(args.program)]
    if args.threads:
        command.append("--threads")
    command += ["--run", "--async", "--settle-ms=5000"] + fonts
    command.append("http://web-platform.test:%d/%s" % (args.port, test))
    environment = dict(os.environ, LD_PRELOAD=str(shim))
    try:
        process = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=60,
                                 cwd=ROOT, env=environment)
        output = process.stdout
    except subprocess.TimeoutExpired as error:
        output = error.stdout.decode("utf-8", "replace") if isinstance(error.stdout, bytes) else (error.stdout or "")
    for line in output.splitlines():
        if line.startswith(PREFIX):
            result = json.loads(line[len(PREFIX):])
            passed = sum(1 for t in result["tests"] if t["status"] == 0)
            return test, "%s %s %d/%d" % (test, HARNESS.get(result["status"], "?"), passed, len(result["tests"]))
    return test, "%s NO-RESULT 0/0" % test


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--program", type=pathlib.Path, default=ROOT / "build/ws345-host/plain/browser")
    parser.add_argument("--out", type=pathlib.Path, default=ROOT / "build/tmp/ws345-wpt")
    parser.add_argument("--port", type=int, default=18310)
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--threads", action="store_true")
    parser.add_argument("tests", nargs="*")
    args = parser.parse_args()
    args.out = args.out.resolve()
    args.program = args.program.resolve()
    if not (SUITE / "wpt").is_file():
        print("wpt-cross-origin: no suite at %s (plan/ws074/tests/fetch-suites.sh wpt)" % SUITE, file=sys.stderr)
        return 2
    args.out.mkdir(parents=True, exist_ok=True)

    # The shim, the report script and the server's config.
    shim = args.out / "wpt-hosts.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O1", "-Wall", "-Wextra", "-Werror", "-o", str(shim),
                    str(ROOT / "plan/ws345/tests/wpt-hosts.c"), "-ldl"], check=True)
    (args.out / "report").mkdir(exist_ok=True)
    (args.out / "report/testharnessreport.js").write_text(REPORT)
    port = args.port
    config = {
        "ports": {"http": [port, port + 1], "http-local": [port + 2], "http-public": [port + 3],
                  "https": [port + 33, port + 34], "https-local": [port + 35], "https-public": [port + 36],
                  "ws": [port + 10], "wss": [port + 11], "h2": [port + 12]},
        "check_subdomains": True,
        "aliases": [{"url-path": "/resources/testharnessreport.js", "local-dir": str(args.out / "report")}],
    }
    (args.out / "config.json").write_text(json.dumps(config))

    # The server, until it says its http port is up.
    log = open(args.out / "serve.log", "w")
    server = subprocess.Popen([sys.executable, "wpt", "serve", "--config", str(args.out / "config.json"), "--no-h2"],
                              cwd=SUITE, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, LD_PRELOAD=str(shim)),
                              start_new_session=True)
    try:
        for _ in range(120):
            time.sleep(0.5)
            if "Starting http server on http://web-platform.test:%d" % port in (args.out / "serve.log").read_text():
                break
        else:
            print("wpt-cross-origin: the server did not start (log %s)" % (args.out / "serve.log"))
            return 1
        time.sleep(2)

        # The tests.
        tests = args.tests or subset()
        lines = {}
        with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
            for test, line in pool.map(lambda t: run_one(args, shim, t), tests):
                lines[test] = line
                print(line, flush=True)
    finally:
        os.killpg(server.pid, signal.SIGTERM)
        try:
            server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            os.killpg(server.pid, signal.SIGKILL)
        log.close()

    # The totals.
    results = [lines[t] for t in tests]
    with open(args.out / "results.txt", "w") as stream:
        stream.write("\n".join(results) + "\n")
    with_results = [r for r in results if " NO-RESULT " not in r]
    passed = sum(int(r.rsplit(" ", 1)[1].split("/")[0]) for r in results)
    total = sum(int(r.rsplit(" ", 1)[1].split("/")[1]) for r in results)
    print("wpt-cross-origin: %d files, %d with results, %d/%d subtests passed" % (len(results), len(with_results), passed, total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
