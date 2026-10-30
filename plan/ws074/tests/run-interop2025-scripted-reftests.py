#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Capture scripted WPT reftests at the first official ready boundary."""

import functools
import gzip
import hashlib
import http.server
import importlib.util
import json
import re
import secrets
import subprocess
import threading
from pathlib import Path
from urllib.parse import quote, urlsplit

from PIL import Image

ROOT = Path(__file__).resolve().parents[3]
WPT_COMMIT = "2d66b9b7998bb58c336138c178323ddee857b586"
WAIT_PATH = "tools/wptrunner/wptrunner/executors/test-wait.js"
FONTS = ROOT / "userland/desktop/fonts"


def load_tool(name, filename):
    spec = importlib.util.spec_from_file_location(name, Path(__file__).with_name(filename))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


CRASH = load_tool("scripted_wait_protocol", "run-interop2025-crashtests.py")
STATIC = load_tool("scripted_reftest_metadata", "run-wpt-reftests.py")


def static_response_headers(root, resource):
    path = resource_path(root, resource)
    if not path.is_file():
        return [], []
    values, sources = [], []
    for base in (path.parent / "__dir__", path):
        template = base.with_name(base.name + ".sub.headers")
        if template.is_file():
            raise Unsupported("response header substitution is unimplemented")
        header = base.with_name(base.name + ".headers")
        if not header.is_file():
            continue
        data = header.read_bytes()
        for line in data.splitlines():
            if not line:
                continue
            if b":" not in line:
                raise Unsupported("malformed static response header")
            key, value = [part.strip() for part in line.split(b":", 1)]
            if not re.fullmatch(br"[!#$%&'*+.^_`|~0-9A-Za-z-]+", key):
                raise Unsupported("invalid response header name")
            if any(byte < 32 and byte != 9 for byte in value) or b"\x7f" in value:
                raise Unsupported("invalid response header value")
            values.append((key.decode("ascii"), value.decode("latin-1")))
        sources.append(dict(path=str(header.relative_to(root)), sha256=hashlib.sha256(data).hexdigest()))
    return values, sources


class Handler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, _format, *_args):
        pass

    def prepare_headers(self):
        self.extra_headers, self.header_sources, self.header_error = [], [], None
        try:
            self.extra_headers, self.header_sources = static_response_headers(Path(self.directory).resolve(), self.path)
        except Unsupported as error:
            self.header_error = str(error)

    def send_header(self, keyword, value):
        # Metadata is the complete source for an explicitly provided field.
        if keyword.lower() in {key.lower() for key, _value in getattr(self, "extra_headers", [])}:
            return
        return super().send_header(keyword, value)

    def end_headers(self):
        for key, value in getattr(self, "extra_headers", []):
            super().send_header(key, value)
        super().end_headers()
        if hasattr(self.server, "header_records"):
            self.server.header_records.append(dict(request=self.path, method=self.command,
                applied=getattr(self, "header_error", None) is None,
                error=getattr(self, "header_error", None),
                sources=getattr(self, "header_sources", []), values=getattr(self, "extra_headers", [])))

    def send_head(self):
        self.prepare_headers()
        return super().send_head()

    def do_HEAD(self):
        if hasattr(self.server, "requests"):
            self.server.requests.append(self.path)
        return super().do_HEAD()

    def do_GET(self):
        if hasattr(self.server, "requests"):
            self.server.requests.append(self.path)
        entry = self.server.probes.get(self.path)
        if entry is None:
            return super().do_GET()
        self.prepare_headers()
        body, mime = entry
        self.send_response(200)
        self.send_header("Content-Type", mime)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


class Server(http.server.ThreadingHTTPServer):
    daemon_threads = True


def wait_script(wpt):
    source = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + WAIT_PATH])
    text = source.decode("utf-8")
    if text.count("%(classname)s") != 1 or "</script" in text.lower():
        raise ValueError("unexpected pinned wait script shape")
    formatted = text.replace("%(classname)s", "reftest-wait")
    return formatted, dict(path=WAIT_PATH, commit=WPT_COMMIT,
                          source_sha256=hashlib.sha256(source).hexdigest(),
                          formatted_sha256=hashlib.sha256(formatted.encode()).hexdigest())


def probe(script, nonce, url, xml=False):
    def literal(value):
        return json.dumps(value).replace("<", "\\u003c")

    marker = "ready-probe-" + nonce
    prefix = """(function () {
var nonce=__NONCE__, originalURL=__URL__, marker=__MARKER__;
var node=document.getElementById(marker);
var removed=false;
if (node) {
  node.parentNode.removeChild(node);
  removed=document.getElementById(marker)===null;
}
if (window.top!==window) return;
function failed(error) {
  console.log(__ERROR__+JSON.stringify({nonce:nonce,message:String(error)}));
}
function start() {
  try {
    (function () {
"""
    suffix = """
    }).apply(window,[originalURL,function(reply) {
      console.log(__READY__+JSON.stringify({nonce:nonce,reply:reply,
        ready_state:document.readyState,probe_removed:removed}));
    }]);
  } catch(error) { failed(error); }
}
function afterLoad() { setTimeout(start,0); }
try {
  if (document.readyState==='complete') afterLoad();
  else addEventListener('load',afterLoad,{once:true});
} catch(error) { failed(error); }
}());"""
    for key, value in [("__NONCE__", nonce), ("__URL__", url), ("__MARKER__", marker),
                       ("__ERROR__", CRASH.ERROR_PREFIX), ("__READY__", CRASH.READY_PREFIX)]:
        prefix = prefix.replace(key, literal(value))
        suffix = suffix.replace(key, literal(value))
    code = prefix + script + suffix
    if xml:
        if "]]>" in code:
            raise ValueError("CDATA boundary in adapter")
        body = '<script xmlns="http://www.w3.org/1999/xhtml" id="%s"><![CDATA[%s]]></script>' % (marker, code)
    else:
        body = '<script id="%s">%s</script>' % (marker, code)
    return body.encode("utf-8")


def inject(original, adapter):
    body, position = CRASH.inject_probe(original, adapter)
    assert body[:position] == original[:position]
    assert body[position + len(adapter):] == original[position:]
    return body, position


def expected_message(nonce, url):
    return CRASH.READY_PREFIX + json.dumps(dict(
        nonce=nonce, reply=[url, "complete", []], ready_state="complete", probe_removed=True),
        separators=(",", ":"), ensure_ascii=False)


def capture_boundary(console, deadline):
    markers = []
    for line in console.splitlines():
        if line.startswith("READY_CAPTURE "):
            markers.append(line)
    if len(markers) != 1:
        return dict(valid=False, markers=markers)
    match = re.fullmatch(r"READY_CAPTURE budget=(\d+) acknowledged=(\d+)", markers[0])
    if match is None:
        return dict(valid=False, markers=markers)
    budget, acknowledged = map(int, match.groups())
    return dict(valid=acknowledged == 1 and 0 <= budget <= deadline,
                budget=budget, acknowledged=acknowledged, markers=markers)


def capture(program, url, output, nonce, deadline=5000, timeout=20, width=800, height=600):
    command = [str(program), url, str(output), expected_message(nonce, url),
               str(FONTS / "Mahora-Regular.ttf"), str(FONTS / "JetBrainsMono-Regular.ttf"),
               str(FONTS / "DroidSansFallbackFull.ttf"), str(width), str(height), str(deadline)]
    timed_out = False
    try:
        run = subprocess.run(command, capture_output=True, timeout=timeout)
        stdout, stderr, returncode = run.stdout, run.stderr, run.returncode
    except subprocess.TimeoutExpired as error:
        stdout, stderr = error.stdout or b"", error.stderr or b""
        returncode, timed_out = None, True
    output.with_suffix(".stdout.bin").write_bytes(stdout)
    output.with_suffix(".stderr.bin").write_bytes(stderr)
    stdout = stdout.decode("utf-8", errors="replace")
    stderr = stderr.decode("utf-8", errors="replace")
    ready = CRASH.parse_readiness(stdout + "\n" + stderr, nonce, url)
    if ready["status"] == "ready" and ready["complete"][0].get("probe_removed") is not True:
        ready["status"] = "adapter_present"
    image = CRASH.image_details(output)
    if image is not None:
        try:
            with Image.open(output) as decoded:
                decoded.load()
        except (OSError, ValueError):
            image = None
    outcome = CRASH.classify(returncode, timed_out, image is not None, ready)
    boundary = capture_boundary(stderr, deadline)
    if outcome == "pass" and not boundary["valid"]:
        outcome = "capture_boundary_invalid"
    if outcome == "pass" and image["size"] != [width, height]:
        outcome = "capture_size_invalid"
    return dict(url=url, nonce=nonce, outcome=outcome, readiness=ready, image=image,
                returncode=returncode, timed_out=timed_out, stdout=stdout, stderr=stderr, command=command,
                capture_boundary=boundary)


def retain_capture(directory, stem, record):
    for stream in ("stdout", "stderr"):
        (directory / (stem + "." + stream)).write_text(record[stream])
    if record["image"]:
        source = Path(record["image"]["file"])
        with Image.open(source) as image:
            rgb = image.convert("RGB")
            pixels = rgb.tobytes()
            target = directory / (stem + ".png")
            rgb.save(target)
        with Image.open(target) as image:
            if image.convert("RGB").tobytes() != pixels:
                raise ValueError("lossless image read-back differs")
        record["png"] = dict(file=str(target), sha256=hashlib.sha256(target.read_bytes()).hexdigest(),
                             rgb_sha256=hashlib.sha256(pixels).hexdigest())
    (directory / (stem + ".json")).write_text(json.dumps(record, indent=2) + "\n")


class Unsupported(RuntimeError):
    pass


class CaptureFailure(RuntimeError):
    def __init__(self, outcome, resource):
        super().__init__(outcome + ": " + resource)
        self.outcome = outcome


def resolve_id(current, href):
    from urllib.parse import urljoin
    resolved = urlsplit(urljoin("http://wpt.invalid" + current, href))
    if resolved.scheme != "http" or resolved.netloc != "wpt.invalid":
        raise Unsupported("external reference")
    value = resolved.path
    if resolved.query:
        value += "?" + resolved.query
    if resolved.fragment:
        value += "#" + resolved.fragment
    return value


def fuzzy_ranges(values, current, reference, references, relation=None):
    # Manifest metadata selects an exact reference key before its unkeyed default.
    parsed = {}
    for content in values:
        key = None
        if ":" in content:
            prefix, content = content.rsplit(":", 1)
            key = resolve_id(current, prefix)
            candidates = [kind for kind, resource in references if resource == key]
            if not candidates:
                raise Unsupported("fuzzy key is not a declared reference")
            key = (key, candidates[0])
        if key in parsed:
            raise Unsupported("duplicate fuzzy key")
        fields = content.split(";")
        if len(fields) != 2:
            raise Unsupported("fuzzy metadata needs two ranges")
        named, positional = {}, []
        for field in fields:
            name = None
            if "=" in field:
                name, field = [part.strip() for part in field.split("=", 1)]
                if name not in ("maxDifference", "totalPixels") or name in named:
                    raise Unsupported("invalid fuzzy field")
            match = re.fullmatch(r"\s*(\d+)\s*(?:-\s*(\d+)\s*)?", field)
            if match is None:
                raise Unsupported("invalid fuzzy range")
            lower = int(match.group(1))
            upper = int(match.group(2) or match.group(1))
            if lower > upper:
                raise Unsupported("reversed fuzzy range")
            if name:
                named[name] = (lower, upper)
            else:
                positional.append((lower, upper))
        ranges = []
        for name in ("maxDifference", "totalPixels"):
            if name in named:
                ranges.append(named.pop(name))
            elif positional:
                ranges.append(positional.pop(0))
            else:
                raise Unsupported("missing fuzzy field")
        if named or positional:
            raise Unsupported("unconsumed fuzzy field")
        parsed[key] = ranges
    if relation is None:
        relation = next((kind for kind, resource in references if resource == reference), None)
    return parsed.get((reference, relation), parsed.get(None))


def compare_images(left, right, fuzzy):
    from PIL import ImageChops, ImageStat
    if left.size != right.size:
        raise Unsupported("capture dimensions differ")
    diff = ImageChops.difference(left.convert("RGB"), right.convert("RGB"))
    box = diff.getbbox()
    if box is None:
        maximum, pixels = 0, 0
    else:
        # The pinned executor measures its luminance mask with ImageStat.
        cropped = diff.crop(box)
        stat = ImageStat.Stat(cropped, cropped.convert("L", dither=None))
        maximum = max(high for _low, high in stat.extrema)
        pixels = stat.count[0]
    if fuzzy is None or fuzzy == [(0, 0), (0, 0)]:
        equal = pixels == 0 and maximum == 0
    else:
        channel, count = fuzzy
        equal = ((pixels == 0 and count[0] == 0) or
                 (maximum == 0 and channel[0] == 0) or
                 (channel[0] <= maximum <= channel[1] and count[0] <= pixels <= count[1]))
    return equal, dict(max_difference=maximum, different_pixels=pixels, fuzzy=fuzzy)


def evaluate_graph(root, get_node, get_capture, max_depth=16):
    edges = []

    def descend(current, ancestry):
        if len(ancestry) > max_depth:
            raise Unsupported("reference depth bound")
        node = get_node(current)
        left = get_capture(current)
        if left["outcome"] != "pass":
            raise CaptureFailure(left["outcome"], current)
        if not node["references"]:
            return True
        for relation, reference in node["references"]:
            right = get_capture(reference)
            if right["outcome"] != "pass":
                raise CaptureFailure(right["outcome"], reference)
            fuzzy = fuzzy_ranges(node["fuzzy"], current, reference, node["references"], relation)
            with Image.open(left["image"]["file"]) as a, Image.open(right["image"]["file"]) as b:
                equal, metrics = compare_images(a, b, fuzzy)
            passed = equal if relation == "match" else not equal
            edge = dict(source=current, reference=reference, relation=relation,
                        equal=equal, passed=passed, **metrics)
            edges.append(edge)
            if not passed:
                continue
            if reference in ancestry:
                raise Unsupported("reference cycle")
            if descend(reference, ancestry + [reference]):
                return True
        return False

    if not get_node(root)["references"]:
        raise Unsupported("selected reftest has no reference")
    try:
        passed = descend(root, [root])
    except (Unsupported, CaptureFailure) as error:
        error.edges = edges
        raise
    return passed, edges


def resource_path(wpt, resource):
    from urllib.parse import unquote
    path = (wpt / unquote(urlsplit(resource).path).lstrip("/")).resolve()
    try:
        path.relative_to(wpt)
    except ValueError as error:
        raise Unsupported("resource leaves WPT root") from error
    return path


def request_limits(wpt, resources, header_records=None, instrumented=None):
    from urllib.parse import parse_qs
    limits = []
    acknowledgements = {}
    for record in header_records or []:
        acknowledgements.setdefault(record["request"], []).append(record)
    for resource in resources:
        try:
            path = resource_path(wpt, resource)
        except Unsupported:
            limits.append([resource, "resource_outside_root"])
            continue
        if path.name in ("testdriver.js", "testdriver-vendor.js"):
            limits.append([resource, "testdriver_remote_actions"])
        if ".sub." in path.name or path.suffix in (".py", ".asis"):
            limits.append([resource, "generated_server_resource"])
        if "pipe" in parse_qs(urlsplit(resource).query):
            limits.append([resource, "response_pipeline"])
        records = acknowledgements.get(resource, [])
        acknowledgement = records.pop(0) if records else None
        try:
            values, sources = static_response_headers(wpt, resource)
        except Unsupported as error:
            limits.append([resource, "unsupported_response_headers:" + str(error)])
            continue
        if sources and (acknowledgement is None or not acknowledgement["applied"] or
                        acknowledgement["sources"] != sources or
                        [list(item) for item in acknowledgement["values"]] != [list(item) for item in values]):
            limits.append([resource, "response_headers_not_acknowledged"])
        if resource == instrumented and any(key.lower() in ("content-length", "content-encoding", "transfer-encoding") for key, _value in values):
            limits.append([resource, "instrumented_representation_header"])
    return limits


def apply_environment(record, root, resources, header_records, instrumented):
    limits = request_limits(root, resources, header_records, instrumented)
    if ".https." in Path(urlsplit(instrumented).path).name:
        limits.append([instrumented, "https_environment"])
    if limits and record["outcome"] == "pass":
        record["outcome"] = "unsupported_environment"
    record["unsupported"] = limits
    record["response_headers"] = header_records
    return limits


def library_digest(program):
    """The engine next to the program, or None for a program without one (the Chromium adapter)."""
    library = program.parent / "libbrowser.so"
    if not library.is_file():
        return None
    return hashlib.sha256(library.read_bytes()).hexdigest()


def run(args):
    import time
    from urllib.parse import urlunsplit
    wpt = args.wpt.resolve()
    STATIC.WPT = wpt
    runner_source = Path(__file__).read_bytes()
    runner_digest = hashlib.sha256(runner_source).hexdigest()
    revision = subprocess.check_output(["git", "-C", str(wpt), "rev-parse", "HEAD"], text=True).strip()
    manifest = json.loads(args.manifest.read_text())
    if revision != WPT_COMMIT or manifest["wpt_commit"] != WPT_COMMIT:
        raise ValueError("fixed WPT revision differs")
    static, _counts, omitted = STATIC.discover(".", manifest["selected"])
    omitted_ids = {item["id"] for item in omitted}
    rows = [item for item in manifest["selected"] if item["id"] in omitted_ids]
    static_ids = {"/" + item["path"] for item in static}
    all_ids = {item["id"] for item in manifest["selected"] if item["type"] == "reftest"}
    if (len(rows) != 413 or len(static_ids) != 1561 or static_ids & omitted_ids or
            static_ids | omitted_ids != all_ids or len(all_ids) != 1974):
        raise ValueError("fixed scripted/static partition differs")
    if args.only:
        # A part of the partition (for a quick look): the checks above ran on the whole selection.
        wanted = set(args.only.split(","))
        rows = [row for row in rows if row["id"] in wanted]
    args.out.mkdir(parents=True, exist_ok=True)
    raw = Path(__import__("tempfile").mkdtemp(prefix="retained-", dir=args.out)).resolve()
    (raw / "runner-source.py").write_bytes(runner_source)
    script, wait_metadata = wait_script(wpt)
    server = Server(("127.0.0.1", 0), functools.partial(Handler, directory=str(wpt)))
    server.probes, server.requests, server.header_records = {}, [], []
    base = "http://127.0.0.1:%d" % server.server_port
    worker = threading.Thread(target=server.serve_forever, daemon=True)
    worker.start()
    nodes, results, capture_records = {}, [], []
    progress = raw / "progress.jsonl"
    started = time.monotonic()

    def checkpoint(record):
        with progress.open("a") as stream:
            stream.write(json.dumps(record) + "\n")
            stream.flush()
            __import__("os").fsync(stream.fileno())

    def get_node(resource):
        if resource in nodes:
            return nodes[resource]
        path = resource_path(wpt, resource)
        if path.suffix.lower() not in (".html", ".htm", ".xht", ".xhtml"):
            raise Unsupported("unimplemented reference document format")
        original = path.read_bytes()
        relative = path.relative_to(wpt).as_posix()
        pinned = subprocess.check_output(["git", "-C", str(wpt), "show", WPT_COMMIT + ":" + relative])
        if original != pinned:
            raise ValueError("resource differs from pinned bytes: " + relative)
        metadata = STATIC.ReftestMetadata()
        metadata.feed(original.decode("utf-8", errors="replace"))
        if metadata.flags.intersection({"animated", "interact", "paged", "speech"}):
            raise Unsupported("unimplemented reference flag")
        node = dict(path=path, original=original, fuzzy=metadata.fuzzy,
                    references=[(relation, resolve_id(resource, href)) for relation, href in metadata.references])
        nodes[resource] = node
        return node

    try:
        checkpoint(dict(kind="header", selected=413, static=1561, union=1974,
                        selected_sha256=hashlib.sha256("\n".join(sorted(omitted_ids)).encode()).hexdigest()))
        for row in rows:
            if time.monotonic() - started >= args.budget_seconds:
                break
            checkpoint(dict(kind="start", test_id=row["id"]))
            cached = {}

            def get_capture(resource):
                if resource in cached:
                    return cached[resource]
                node = get_node(resource)
                wire = quote(resource, safe="/?=&%")
                url = base + wire
                parsed = urlsplit(wire)
                request = urlunsplit(("", "", parsed.path, parsed.query, ""))
                nonce = secrets.token_hex(16)
                adapter = probe(script, nonce, url, xml=node["path"].suffix.lower() in (".xht", ".xhtml"))
                body, position = inject(node["original"], adapter)
                stem = "capture-%05d" % (len(capture_records) + 1)
                (raw / (stem + ".instrumented")).write_bytes(body)
                server.probes = {request: (body, "application/xhtml+xml" if node["path"].suffix.lower() in (".xht", ".xhtml") else "text/html")}
                first_request = len(server.requests)
                first_header = len(server.header_records)
                record = capture(args.program, url, raw / (stem + ".ppm"), nonce,
                                 deadline=args.deadline_ms, timeout=args.process_timeout)
                requests = server.requests[first_request:]
                header_records = server.header_records[first_header:]
                limits = apply_environment(record, wpt, requests, header_records, request)
                record.update(resource=resource, record=stem + ".json", requests=requests, response_headers=header_records, unsupported=limits,
                              source_sha256=hashlib.sha256(node["original"]).hexdigest(),
                              instrumented_sha256=hashlib.sha256(body).hexdigest(), insertion_offset=position)
                retain_capture(raw, stem, record)
                capture_records.append(dict(resource=resource, record=stem + ".json", outcome=record["outcome"]))
                cached[resource] = record
                return record

            record = dict(test_id=row["id"], focus=row["focus"], captures=[])
            try:
                passed, edges = evaluate_graph(row["id"], get_node, get_capture)
                record.update(outcome="pass" if passed else "fail", edges=edges)
            except CaptureFailure as error:
                record.update(outcome=error.outcome, error=str(error), edges=getattr(error, "edges", []))
            except (Unsupported, FileNotFoundError) as error:
                record.update(outcome="unsupported", error=str(error), edges=getattr(error, "edges", []))
            record["captures"] = [dict(resource=key, outcome=value["outcome"],
                                        record=value["record"]) for key, value in cached.items()]
            results.append(record)
            checkpoint(dict(kind="result", record=record))
            print("[%d/413] %s %s" % (len(results), record["outcome"], record["test_id"]), flush=True)
    finally:
        server.shutdown()
        server.server_close()
        worker.join()
    counts = {}
    for record in results:
        counts[record["outcome"]] = counts.get(record["outcome"], 0) + 1
    report = dict(schema=1, pass_semantics="native_first_official_ready_and_reference_leaf",
                  results=results, captures=capture_records, counts=counts,
                  selected=413, completed=len(results), unexecuted=[row["id"] for row in rows[len(results):]],
                  raw_directory=str(raw), checkpoint=str(progress), wait_script=wait_metadata,
                  wpt_commit=revision, metadata_commit=manifest["metadata_commit"],
                  viewport=[800, 600], deadline_ms=args.deadline_ms, process_timeout=args.process_timeout,
                  program_sha256=hashlib.sha256(args.program.read_bytes()).hexdigest(),
                  engine_library_sha256=library_digest(args.program),
                  partition=dict(scripted=413, static=1561, union=1974),
                  manifest_sha256=hashlib.sha256(args.manifest.read_bytes()).hexdigest(),
                  runner_sha256=runner_digest,
                  source_changed_during_run=hashlib.sha256(Path(__file__).read_bytes()).hexdigest() != runner_digest,
                  selected_sha256=hashlib.sha256("\n".join(sorted(omitted_ids)).encode()).hexdigest(),
                  captured_cases=sum(bool(row["captures"]) for row in results),
                  seconds=time.monotonic() - started)
    (args.out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(counts)
    return 0


def main():
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--wpt", type=Path, default=ROOT / "build/ws074-suites/wpt")
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--program", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--deadline-ms", type=int, default=5000)
    parser.add_argument("--process-timeout", type=int, default=20)
    parser.add_argument("--budget-seconds", type=int, default=2400)
    parser.add_argument("--only", default="", help="comma-separated test IDs to run, a part of the partition")
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
