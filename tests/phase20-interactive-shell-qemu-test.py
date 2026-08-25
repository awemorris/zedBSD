#!/usr/bin/env python3
"""Prove that an interactive shell regains its tty after ifconfig exits."""

import argparse
from pathlib import Path
import socket
import subprocess
import tempfile
import time


PROMPT = "root@zedbsd:/root$ "


def wait_for(log, predicate, deadline, description):
    output = ""
    while time.monotonic() < deadline:
        output = log.read_text(errors="replace") if log.exists() else ""
        if predicate(output):
            return output
        time.sleep(0.05)
    raise RuntimeError(f"timed out waiting for {description}\n{output[-12000:]}")


def connect_monitor(path, process, deadline):
    while time.monotonic() < deadline:
        monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            monitor.connect(str(path))
            monitor.settimeout(0.2)
            try:
                monitor.recv(4096)
            except socket.timeout:
                pass
            return monitor
        except OSError:
            monitor.close()
            if process.poll() is not None:
                raise RuntimeError("QEMU exited before its monitor was ready")
            time.sleep(0.05)
    raise RuntimeError("timed out connecting to the QEMU monitor")


def send_text(monitor, text):
    key_names = {" ": "spc", "\n": "ret"}
    commands = []
    for character in text:
        key = key_names.get(character, character)
        if not (key.isalnum() or key in ("spc", "ret")):
            raise ValueError(f"unsupported test key: {character!r}")
        commands.append(f"sendkey {key}\n")
    monitor.sendall("".join(commands).encode())


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", required=True)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=60)
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="zedbsd-interactive-qemu-") as work:
        work_path = Path(work)
        log = work_path / "debugcon.log"
        monitor_path = work_path / "monitor.sock"
        command = (
            args.qemu,
            "-machine",
            "pc",
            "-m",
            "512",
            "-smp",
            "4",
            "-display",
            "none",
            "-serial",
            "none",
            "-monitor",
            f"unix:{monitor_path},server=on,wait=off",
            "-no-reboot",
            "-debugcon",
            f"file:{log}",
            "-drive",
            f"file={args.image.resolve()},format=raw,if=ide",
            "-boot",
            "c",
        )
        process = subprocess.Popen(command)
        deadline = time.monotonic() + args.timeout
        monitor = None
        try:
            monitor = connect_monitor(monitor_path, process, deadline)
            wait_for(log, lambda output: "login: " in output, deadline, "login")
            send_text(monitor, "root\n")
            wait_for(log, lambda output: "Password: " in output, deadline, "password")
            send_text(monitor, "\n")
            output = wait_for(log, lambda text: PROMPT in text, deadline, "shell prompt")
            prompt_count = output.count(PROMPT)

            send_text(monitor, "ifconfig\n")
            output = wait_for(
                log,
                lambda text: "TX packets" in text and text.count(PROMPT) > prompt_count,
                deadline,
                "ifconfig completion",
            )
            time.sleep(0.5)
            output = log.read_text(errors="replace")
            if output.count(PROMPT) != prompt_count + 1:
                raise RuntimeError("shell prompt repeated after ifconfig\n" + output[-12000:])

            send_text(monitor, "echo interactivealive\n")
            output = wait_for(
                log,
                lambda text: "\ninteractivealive\n" in text
                and text.count(PROMPT) == prompt_count + 2,
                deadline,
                "post-ifconfig interactive command",
            )
            send_text(monitor, "poweroff\n")
            wait_for(
                log,
                lambda text: "init: stopping services" in text,
                deadline,
                "orderly shutdown",
            )
        except Exception as error:
            output = log.read_text(errors="replace") if log.exists() else ""
            raise SystemExit(f"{error}\n{output[-12000:]}") from error
        finally:
            if monitor is not None:
                monitor.close()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()

    print("zedBSD interactive ifconfig shell QEMU test: PASS")


if __name__ == "__main__":
    main()
