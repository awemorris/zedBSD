#!/usr/bin/env python3
"""Check actual standard Vulkan readback with the independent cuboid oracle.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

The executable uses normal Vulkan loader configuration, including an optional
VK_DRIVER_FILES chosen by the caller. No mock GPU or recorded image is accepted.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import selectors
import shutil
import subprocess
import sys
import time

from vkdemo_oracle import read_ppm, verify_pixels


def run(binary, output):
    output.mkdir(parents=True, exist_ok=True)
    frame = output / "current.ppm"
    transcript = []
    frames = []
    marker = re.compile(
        r"^VKDEMO OFFSCREEN run=standard-vulkan mode=(fixed|live) "
        r"sample=(\d+) frame=(\d+) time_ms=(\d+) rgb_sha256=([0-9a-f]{64}) "
        r"width=320 height=240$"
    )
    process = subprocess.Popen(
        [str(binary), "--offscreen", "--verify-session",
         "--token=standard-vulkan", "--output=" + str(frame)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=False, bufsize=0,
    )
    selector = selectors.DefaultSelector()
    selector.register(process.stdout, selectors.EVENT_READ)
    deadline = time.monotonic() + 90
    pending = b""
    done = False
    try:
        while not done:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError("standard Vulkan checkpoint run exceeded 90 seconds")
            events = selector.select(min(remaining, 1))
            if not events:
                if process.poll() is not None:
                    raise RuntimeError("app exited before its six-frame completion marker")
                continue
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError("app output ended before its completion marker")
            pending += chunk
            while b"\n" in pending:
                line, pending = pending.split(b"\n", 1)
                line = line.decode("utf-8", errors="replace")
                transcript.append(line)
                print(line, flush=True)
                match = marker.fullmatch(line)
                if match:
                    mode, sample, index, milliseconds, digest = match.groups()
                    index, sample, milliseconds = map(int, (index, sample, milliseconds))
                    if index != len(frames) + 1 or index > 6:
                        raise RuntimeError("checkpoint identity was repeated or skipped")
                    expected_mode = "fixed" if index <= 3 else "live"
                    expected_sample = (index - 1) % 3 + 1
                    if mode != expected_mode or sample != expected_sample:
                        raise RuntimeError("checkpoint mode or sample differs from its contract")
                    if mode == "fixed" and milliseconds != (0, 1000, 2500)[sample - 1]:
                        raise RuntimeError("fixed checkpoint changed its required shader time")
                    pixels = read_ppm(frame)
                    if hashlib.sha256(pixels).hexdigest() != digest:
                        raise RuntimeError("PPM readback and GPU marker hashes differ")
                    result = verify_pixels(pixels, milliseconds)
                    if not result["passed"]:
                        raise RuntimeError("independent pixel oracle rejected " + json.dumps(result))
                    destination = output / f"frame-{index:02d}.ppm"
                    shutil.copyfile(frame, destination)
                    result.update(mode=mode, sample=sample, frame=index, file=destination.name)
                    frames.append(result)
                    process.stdin.write(b"\n")
                    process.stdin.flush()
                elif line == "VKDEMO DONE run=standard-vulkan frames=6":
                    done = True
                elif "VKDEMO FAILED" in line:
                    raise RuntimeError(line)
        status = process.wait(timeout=10)
        if status != 0 or len(frames) != 6:
            raise RuntimeError("the six-frame session did not close successfully")
    finally:
        selector.close()
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        (output / "checkpoint.log").write_text("\n".join(transcript) + "\n")
    live_times = [item["time_ms"] for item in frames if item["mode"] == "live"]
    if not live_times[0] < live_times[1] < live_times[2]:
        raise RuntimeError("live shader times did not advance after captures")
    second = subprocess.run(
        [str(binary), "--offscreen", "--duration=2", "--token=reopen"],
        capture_output=True, text=True, timeout=30,
    )
    (output / "reopen.log").write_text(second.stdout + second.stderr)
    if second.returncode != 0 or "VKDEMO DONE run=reopen frames=" not in second.stdout:
        raise RuntimeError("ordinary second invocation failed after complete cleanup")
    result = {
        "passed": True, "api": "standard Vulkan 1.0", "mode": "offscreen",
        "display_verified": False, "driver_files": os.environ.get("VK_DRIVER_FILES"),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "frames": frames, "reopen_returncode": second.returncode,
        "device": next((line for line in transcript if line.startswith("VKDEMO VULKAN ")), None),
    }
    (output / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print("standard Vulkan vkdemo: PASS (six oracle frames, normal close, second run)")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        raise SystemExit("usage: vkdemo-vulkan-test.py BINARY OUTPUT_DIRECTORY")
    run(Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve())
