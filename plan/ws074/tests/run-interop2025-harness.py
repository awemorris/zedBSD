#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Capture selected physical and official generated Window testharness pages.

One HTTP origin preserves original files and adds only reporting callbacks.
Generated contexts use pinned WPT templates; unsupported environments stay non-PASS.
"""

import argparse
import functools
import hashlib
import http.server
import json
from pathlib import Path
import subprocess
import sys
import threading
from urllib.parse import urlsplit


ROOT = Path(__file__).resolve().parents[3]
WPT_COMMIT = "2d66b9b7998bb58c336138c178323ddee857b586"
RESULT_PREFIX = "WS074_WPT_RESULT "
COMPLETE_PREFIX = "WS074_WPT_COMPLETE "
REPORT_SCRIPT = r"""
(function () {
    function test_result(test) {
        return {name: String(test.name), status: test.status,
                message: test.message ? String(test.message) : ""};
    }
    add_result_callback(function (test) {
        console.log("WS074_WPT_RESULT " + JSON.stringify(test_result(test)));
    });
    add_completion_callback(function (tests, harness_status) {
        var results = [];
        for (var i = 0; i < tests.length; i++) {
            results.push(test_result(tests[i]));
        }
        console.log("WS074_WPT_COMPLETE " + JSON.stringify({
            tests: results,
            status: harness_status.status,
            message: harness_status.message ? String(harness_status.message) : ""
        }));
    });
}());
"""
TEST_STATUSES = {0: "PASS", 1: "FAIL", 2: "TIMEOUT", 3: "NOTRUN",
                 4: "PRECONDITION_FAILED"}
HARNESS_STATUSES = {0: "OK", 1: "ERROR", 2: "TIMEOUT",
                    3: "PRECONDITION_FAILED"}
FONT_NAMES = ("Mahora-Regular.ttf", "JetBrainsMono-Regular.ttf",
              "DroidSansFallbackFull.ttf")


def font_arguments(directory):
    """Pass a complete explicit font set to pages that need layout."""
    if directory is None:
        return []
    paths = [directory / name for name in FONT_NAMES]
    if not all(path.is_file() for path in paths):
        raise ValueError("fixed font directory is missing a required font")
    return ["--font=" + str(paths[0]), "--mono-font=" + str(paths[1]),
            "--fallback-font=" + str(paths[2])]


class WptHandler(http.server.SimpleHTTPRequestHandler):
    """Serve the pinned WPT tree, adding only harness reporting callbacks."""

    def log_message(self, _format, *args):
        del args

    def do_GET(self):
        if self.path.split("?", 1)[0] != "/resources/testharnessreport.js":
            return super().do_GET()
        original = (Path(self.directory) / "resources/testharnessreport.js").read_bytes()
        body = original + b"\n" + REPORT_SCRIPT.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/javascript; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class WptServer(http.server.ThreadingHTTPServer):
    daemon_threads = True


def classify_output(stdout, returncode, timed_out=False):
    """Separate transport failures, missing completion and WPT test results."""
    results = []
    complete = []
    errors = []
    for line in stdout.splitlines():
        try:
            if line.startswith(RESULT_PREFIX):
                results.append(json.loads(line[len(RESULT_PREFIX):]))
            elif line.startswith(COMPLETE_PREFIX):
                complete.append(json.loads(line[len(COMPLETE_PREFIX):]))
        except json.JSONDecodeError as error:
            errors.append(str(error))
    if timed_out:
        outcome = "process_timeout"
    elif returncode != 0:
        outcome = "process_error"
    elif errors:
        outcome = "invalid_result_json"
    elif not stdout.strip():
        outcome = "no_output"
    elif len(complete) != 1:
        outcome = "no_completion" if not complete else "multiple_completions"
    else:
        status = complete[0].get("status")
        tests = complete[0].get("tests")
        if status not in HARNESS_STATUSES or not isinstance(tests, list):
            outcome = "invalid_completion"
        elif not all(isinstance(test, dict) for test in tests):
            outcome = "invalid_test_result"
        elif status == 2 or any(test.get("status") == 2 for test in tests):
            outcome = "timeout"
        elif status != 0:
            outcome = "harness_error"
        elif not tests:
            outcome = "zero_tests"
        elif any(test.get("status") not in TEST_STATUSES for test in tests):
            outcome = "invalid_test_status"
        elif any(test["status"] != 0 for test in tests):
            outcome = "fail"
        else:
            outcome = "pass"
    return {"outcome": outcome, "result_callbacks": results,
            "completion": complete[0] if len(complete) == 1 else None,
            "json_errors": errors}


def generated_source(row):
    """Return the physical JS source only for the selected plain Window family."""
    address = urlsplit(row["id"])
    path = address.path
    if address.scheme or address.netloc or not path.startswith("/"):
        return None
    if ".https." in path or ".sub." in path:
        return None
    for suffix, replacement in ((".window.html", ".window.js"), (".any.html", ".any.js")):
        if path.endswith(suffix):
            source = path[:-len(suffix)] + replacement
            if row["source"] == source.lstrip("/"):
                return source
    return None


def generated_contract(wpt):
    """Read immutable wrapper templates and metadata definitions as pinned data."""
    import ast
    import re
    sources = {}
    for path in ("tools/serve/serve.py", "tools/manifest/sourcefile.py", "tools/wptserve/wptserve/server.py",
                 "tools/wptserve/wptserve/handlers.py", "tools/wptserve/wptserve/constants.py"):
        sources[path] = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + path])
    templates = {}
    rewrites = None
    for node in ast.parse(sources["tools/serve/serve.py"]).body:
        if isinstance(node, ast.ClassDef) and node.name in ("WindowHandler", "AnyHtmlHandler"):
            for member in node.body:
                if isinstance(member, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "wrapper" for t in member.targets):
                    templates[node.name] = ast.literal_eval(member.value)
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "rewrites" for t in node.targets):
            rewrites = ast.literal_eval(node.value)
    pattern, variants, defaults = None, None, None
    for node in ast.parse(sources["tools/manifest/sourcefile.py"]).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "js_meta_re" for t in node.targets):
            pattern = ast.literal_eval(node.value.args[0])
        if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name) and node.target.id == "_any_variants":
            variants = ast.literal_eval(node.value)
        if isinstance(node, ast.FunctionDef) and node.name == "get_default_any_variants":
            statement = next(item for item in node.body if isinstance(item, ast.Return))
            defaults = ast.literal_eval(statement.value.args[0])
    if set(templates) != {"WindowHandler", "AnyHtmlHandler"} or pattern is None or variants is None or defaults is None:
        raise ValueError("pinned generated-wrapper contract is incomplete")
    if rewrites != [("GET", "/resources/WebIDLParser.js", "/resources/webidl2/lib/webidl2.js")]:
        raise ValueError("pinned resource rewrite needs an explicit review")
    content_types = None
    for node in ast.parse(sources["tools/wptserve/wptserve/constants.py"]).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == "content_types" for t in node.targets):
            content_types = {extension: mime for mime, extensions in ast.literal_eval(node.value.args[0]).items() for extension in extensions}
    if content_types is None or "idl" in content_types:
        raise ValueError("pinned IDL MIME needs an explicit review")
    paths = subprocess.check_output(["git", "-C", str(wpt), "ls-tree", "-r", "--name-only", "-z", WPT_COMMIT])
    pinned_paths = set(paths.decode("utf-8").rstrip("\0").split("\0"))
    return dict(repo=str(wpt.resolve()), pinned_paths=pinned_paths, templates=templates, pattern=re.compile(pattern), variants=variants, defaults=defaults,
                rewrites=rewrites, provenance={path: hashlib.sha256(data).hexdigest() for path, data in sources.items()})


def generated_metadata(original, contract):
    """Read only the initial contiguous metadata block, including original CR bytes."""
    import io
    entries = []
    for line in io.BytesIO(original):
        match = contract["pattern"].match(line)
        if match is None:
            break
        entries.append(tuple(part.decode("utf-8") for part in match.groups()))
    return entries


def generated_wrapper(identifier, source, original, contract):
    """Expand the official Window scaffold without changing the test's JS bytes."""
    address = urlsplit(identifier)
    kind = "AnyHtmlHandler" if address.path.endswith(".any.html") else "WindowHandler"
    metadata = generated_metadata(original, contract)
    if kind == "AnyHtmlHandler":
        global_value = next((value for key, value in metadata if key == "global"), "")
        exposed = set(contract["defaults"]) if global_value == "" else set()
        if global_value != "":
            for token in global_value.split(","):
                token = token.strip()
                variant = contract["variants"].get(token)
                if variant is not None:
                    exposed.update(variant.get("longhand", {token}))
        if "window" not in exposed:
            raise ValueError("the pinned metadata does not expose a Window context")
    meta, scripts = [], []
    for key, value in metadata:
        if key == "title":
            meta.append("<title>%s</title>" % value.replace("&", "&amp;").replace("<", "&lt;"))
        elif key == "timeout" and value == "long":
            meta.append('<meta name="timeout" content="long">')
        elif key == "script":
            scripts.append('<script src="%s"></script>' % value.replace("&", "&amp;").replace('"', "&quot;"))
    query = "?" + address.query if address.query else ""
    body = contract["templates"][kind] % dict(meta="\n".join(meta), script="\n".join(scripts), path=source, query=query)
    return body.encode("utf-8"), dict(kind=kind, metadata=metadata, source=source,
        source_sha256=hashlib.sha256(original).hexdigest(), wrapper_sha256=hashlib.sha256(body.encode("utf-8")).hexdigest(),
        contract_sources=contract["provenance"])


def generated_rewrite(request, contract, method="GET"):
    from urllib.parse import urlunsplit
    address = urlsplit(request)
    for verb, incoming, destination in contract["rewrites"]:
        if verb == method and address.path == incoming:
            return urlunsplit(address._replace(path=destination))
    return request


def generated_http_tool():
    """Reuse the already verified header provider only in generated contexts."""
    import importlib.util
    spec = importlib.util.spec_from_file_location("interop_generated_headers", Path(__file__).with_name("run-interop2025-scripted-reftests.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def generated_git_resource(path, contract):
    """Provide only missing plain IDL blobs whose pinned header metadata is absent."""
    import re
    if re.fullmatch(r"interfaces/[A-Za-z0-9_.-]+\.idl", path) is None:
        return None
    if path not in contract["pinned_paths"]:
        return None
    metadata = [base + suffix for base in ("interfaces/__dir__", path)
                for suffix in (".headers", ".sub.headers")]
    if any(item in contract["pinned_paths"] for item in metadata):
        raise ValueError("pinned IDL headers require their separate handler contract")
    body = subprocess.check_output(["git", "-C", contract["repo"], "show", WPT_COMMIT + ":" + path])
    record = dict(path=path, sha256=hashlib.sha256(body).hexdigest(),
                  provider="pinned-git-blob", commit=WPT_COMMIT,
                  absent_header_paths=metadata, mime="application/octet-stream")
    return body, record


def generated_server(root, row, contract, raw):
    """Prepare a generated-only provider and retain its exact input artifacts."""
    from urllib.parse import urljoin, urlunsplit
    root = root.resolve()
    source = generated_source(row)
    if source is None:
        raise ValueError("unsupported generated context")
    tool = generated_http_tool()
    source_path = tool.resource_path(root, source)
    for base in (source_path.parent / "__dir__", source_path):
        for suffix in (".headers", ".sub.headers"):
            if base.with_name(base.name + suffix).is_file():
                raise ValueError("wrapper response headers need their separate handler contract")
    original = source_path.read_bytes()
    body, context = generated_wrapper(row["id"], source, original, contract)
    for key, value in context["metadata"]:
        if key != "script":
            continue
        address = urlsplit(urljoin("http://wpt.invalid" + row["id"], value))
        if address.scheme != "http" or address.netloc != "wpt.invalid":
            raise ValueError("external META script is unimplemented")
        resource = urlunsplit(("", "", address.path, address.query, ""))
        target = tool.resource_path(root, generated_rewrite(resource, contract))
        if not target.is_file():
            raise ValueError("required META script is absent: " + resource)
    report_original = (root / "resources/testharnessreport.js").read_bytes()
    report = report_original + b"\n" + REPORT_SCRIPT.encode("utf-8")
    address = urlsplit(row["id"])
    top_request = urlunsplit(("", "", address.path, address.query, ""))

    class GeneratedHandler(tool.Handler):
        def do_GET(self):
            incoming = self.path
            self.server.logical_requests.append(incoming)
            self.path = generated_rewrite(incoming, contract)
            if self.path != incoming:
                self.server.rewrite_records.append(dict(incoming=incoming, destination=self.path))
            if urlsplit(self.path).path == "/resources/testharnessreport.js":
                self.server.probes[self.path] = (report, "text/javascript; charset=utf-8")
            try:
                target = tool.resource_path(root, self.path)
                relative = str(target.relative_to(root))
                if not target.is_file() and self.path not in self.server.probes:
                    try:
                        supplied = generated_git_resource(relative, contract)
                    except ValueError as error:
                        self.server.input_limits.append([self.path, str(error)])
                        supplied = None
                    if supplied is not None:
                        data, record = supplied
                        record["request"] = self.path
                        self.server.probes[self.path] = (data, record["mime"])
                        self.server.pinned_resources.append(record)
                        (raw / ("git-asset-%03d.idl" % len(self.server.pinned_resources))).write_bytes(data)
                    elif relative in contract["pinned_paths"]:
                        self.server.input_limits.append([self.path, "missing_pinned_resource"])
            except tool.Unsupported:
                self.send_error(403)
                self.path = incoming
                return
            try:
                super().do_GET()
            finally:
                self.path = incoming

    raw.mkdir(parents=True, exist_ok=False)
    (raw / "source.js").write_bytes(original)
    (raw / "wrapper.html").write_bytes(body)
    (raw / "testharnessreport.js").write_bytes(report)
    server = WptServer(("127.0.0.1", 0), functools.partial(GeneratedHandler, directory=str(root)))
    server.probes = {top_request: (body, "text/html")}
    server.requests, server.header_records, server.logical_requests, server.rewrite_records = [], [], [], []
    server.pinned_resources, server.input_limits = [], []
    context.update(top_request=top_request, raw_directory=str(raw), report_original_sha256=hashlib.sha256(report_original).hexdigest(),
                   report_instrumented_sha256=hashlib.sha256(report).hexdigest(), header_tool_sha256=hashlib.sha256(Path(tool.__file__).read_bytes()).hexdigest())
    return server, context, tool


def generated_environment(root, server, context, tool):
    """Reject unsupported environment success and audit every served static input."""
    instruments = [context["top_request"]] + [x for x in server.requests if urlsplit(x).path == "/resources/testharnessreport.js"]
    limits = list(server.input_limits)
    for instrument in instruments:
        for limit in tool.request_limits(root, server.requests, server.header_records, instrument):
            if limit not in limits:
                limits.append(limit)
    assets = []
    for request in dict.fromkeys(server.requests):
        path = tool.resource_path(root, request)
        if path.is_file():
            original = path.read_bytes()
            assets.append(dict(request=request, path=str(path.relative_to(root)), sha256=hashlib.sha256(original).hexdigest()))
    assets.extend(server.pinned_resources)
    context.update(requests=server.requests, logical_requests=server.logical_requests, rewrites=server.rewrite_records,
                   response_headers=server.header_records, unsupported=limits, assets=assets)
    raw = Path(context["raw_directory"])
    (raw / "provider.json").write_text(json.dumps(context, indent=2) + "\n")
    return limits


def generated_result(result, root, server, context, tool):
    """Keep successful native callbacks non-PASS when their provider is unsupported."""
    limits = generated_environment(root, server, context, tool)
    result["native_outcome"] = result["outcome"]
    if limits and result["outcome"] == "pass":
        result["outcome"] = "unsupported_environment"
    result["generated"] = context
    return limits


def run(args):
    wpt = args.wpt.resolve()
    revision = subprocess.check_output(
        ["git", "-C", str(wpt), "rev-parse", "HEAD"], text=True,
    ).strip()
    if revision != WPT_COMMIT:
        raise ValueError("WPT revision differs from the fixed input")
    selected = json.loads(args.manifest.read_text(encoding="utf-8"))
    if selected.get("wpt_commit") != WPT_COMMIT:
        raise ValueError("selection manifest has a different WPT revision")
    matches = [row for row in selected["selected"] if row["id"] == args.test_id]
    if len(matches) != 1 or matches[0]["type"] != "testharness":
        raise ValueError("test ID is not a single selected testharness case")
    # Context belongs to the path; selected identity and the capture URL retain the variant.
    address = urlsplit(args.test_id)
    if address.scheme or address.netloc or not address.path.startswith("/"):
        raise ValueError("test ID must be an origin-relative selected path")
    if ".https." in address.path or not address.path.endswith(".html"):
        raise ValueError("this runner supports only plain HTTP HTML paths")
    generated = generated_source(matches[0])
    if generated is None and matches[0]["source"] != address.path.lstrip("/"):
        raise ValueError("generated test context is unsupported")
    if not (wpt / matches[0]["source"]).is_file():
        raise ValueError("selected WPT source is absent")
    if not args.program.is_file():
        raise ValueError("browser program is absent")
    context, header_tool = None, None
    if generated is not None:
        contract = generated_contract(wpt)
        pinned_source = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + matches[0]["source"]])
        if (wpt / matches[0]["source"]).read_bytes() != pinned_source:
            raise ValueError("generated source differs from pinned input")
        raw = (args.out.parent / (args.out.stem + ".generated")).resolve()
        server, context, header_tool = generated_server(wpt, matches[0], contract, raw)
    else:
        server = WptServer(("127.0.0.1", 0), functools.partial(WptHandler, directory=str(wpt)))
    worker = threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
    worker.start()
    url = "http://127.0.0.1:%d%s" % (server.server_port, args.test_id)
    # --async: a frame's page is fetched in the background, as in the window; the at-once mode never loads a web frame.
    command = ([str(args.program), "--run", "--async", "--settle-ms=%d" % args.settle_ms] +
               font_arguments(getattr(args, "font_dir", None)) + [url])
    try:
        try:
            if context is not None:
                process = subprocess.run(command, capture_output=True, timeout=args.process_timeout)
                stdout_binary, stderr_binary = process.stdout, process.stderr
                stdout, stderr = stdout_binary.decode("utf-8", errors="replace"), stderr_binary.decode("utf-8", errors="replace")
            else:
                process = subprocess.run(command, capture_output=True, text=True,
                                         errors="replace", timeout=args.process_timeout)
                stdout, stderr = process.stdout, process.stderr
            returncode, timed_out = process.returncode, False
        except subprocess.TimeoutExpired as error:
            stdout_binary, stderr_binary = error.stdout or b"", error.stderr or b""
            stdout = stdout_binary.decode("utf-8", errors="replace")
            stderr = stderr_binary.decode("utf-8", errors="replace")
            returncode, timed_out = None, True
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
    result = classify_output(stdout, returncode, timed_out)
    if context is not None:
        raw = Path(context["raw_directory"])
        (raw / "stdout.bin").write_bytes(stdout_binary)
        (raw / "stderr.bin").write_bytes(stderr_binary)
        (raw / "stdout.txt").write_text(stdout)
        (raw / "stderr.txt").write_text(stderr)
        generated_result(result, wpt, server, context, header_tool)
        pinned_assets = {}
        for asset in context["assets"]:
            original = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + asset["path"]])
            digest = hashlib.sha256(original).hexdigest()
            if digest != asset["sha256"]:
                raise ValueError("served generated resource differs from pinned input")
            pinned_assets[asset["path"]] = digest
        for response in context["response_headers"]:
            for source in response["sources"]:
                original = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + source["path"]])
                digest = hashlib.sha256(original).hexdigest()
                if digest != source["sha256"]:
                    raise ValueError("served response headers differ from pinned input")
                pinned_assets[source["path"]] = digest
        context["pinned_assets"] = pinned_assets
        (Path(context["raw_directory"]) / "provider.json").write_text(json.dumps(context, indent=2) + "\n")
    result.update({"test_id": args.test_id, "focus": matches[0]["focus"],
                   "wpt_commit": revision, "metadata_commit": selected["metadata_commit"],
                   "program_sha256": hashlib.sha256(args.program.read_bytes()).hexdigest(),
                   "command": command, "returncode": returncode,
                   "stdout": stdout, "stderr": stderr,
                   "settle_ms": args.settle_ms, "process_timeout": args.process_timeout})
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n",
                        encoding="utf-8")
    print("%s: %s (%s)" % (args.test_id, result["outcome"], args.out))
    return 0 if result["outcome"] == "pass" else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wpt", type=Path, default=ROOT / "build/ws074-suites/wpt")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--program", type=Path,
                        default=ROOT / "build/ws074-host/plain/browser")
    parser.add_argument("--test-id", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--settle-ms", type=int, default=5000)
    parser.add_argument("--process-timeout", type=int, default=20)
    parser.add_argument("--font-dir", type=Path,
                        help="directory with the three fixed host fonts for layout")
    args = parser.parse_args()
    if args.settle_ms < 0 or args.settle_ms > 60000 or args.process_timeout < 1:
        parser.error("invalid time budget")
    try:
        return run(args)
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print("run-interop2025-harness: %s" % error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
