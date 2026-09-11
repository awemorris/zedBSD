#!/usr/bin/env python3
"""Exercise the native lp/lpr client against a deterministic LPD peer."""

import os
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading


PDF = b"%PDF-1.4\n% zedBSD LPD test\n%%EOF\n"
ACK_STAGES = (
    "receive-job",
    "data-header",
    "data-body",
    "control-header",
    "control-body",
)


def receive_exact(connection, size):
    """Read exactly size bytes or fail the server fixture."""
    result = bytearray()
    while len(result) < size:
        chunk = connection.recv(size - len(result))
        if not chunk:
            raise AssertionError("LPD peer closed a record early")
        result.extend(chunk)
    return bytes(result)


def receive_line(connection):
    """Read one bounded newline-terminated protocol line."""
    result = bytearray()
    while True:
        value = receive_exact(connection, 1)
        result.extend(value)
        if value == b"\n":
            return bytes(result)
        if len(result) > 1024:
            raise AssertionError("LPD protocol line exceeded test bound")


def parse_header(line, command):
    """Parse one LPD record header and return its size and name."""
    if not line.startswith(bytes((command,))) or not line.endswith(b"\n"):
        raise AssertionError("unexpected LPD record header: %r" % (line,))
    fields = line[1:-1].split(b" ", 1)
    if len(fields) != 2 or not fields[0].isdigit() or not fields[1]:
        raise AssertionError("malformed LPD record header: %r" % (line,))
    return int(fields[0]), fields[1]


class FakeLpd:
    """Accept one client and record its receive-job transaction."""

    def __init__(self, reject=None):
        self.reject = reject
        self.error = None
        self.records = {}
        self.listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.thread = threading.Thread(target=self._run, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, exception_type, exception, traceback):
        self.thread.join(5)
        self.listener.close()
        if self.thread.is_alive():
            raise AssertionError("LPD fixture did not terminate")
        if self.error is not None and exception_type is None:
            raise self.error

    def acknowledge(self, connection, stage):
        """Send the selected acknowledgement and report acceptance."""
        rejected = stage == self.reject
        connection.sendall(b"\x01" if rejected else b"\x00")
        return not rejected

    def _run(self):
        try:
            connection, _ = self.listener.accept()
            with connection:
                line = receive_line(connection)
                if line != b"\x02queue\n":
                    raise AssertionError("unexpected receive-job request: %r" % line)
                if not self.acknowledge(connection, "receive-job"):
                    return

                line = receive_line(connection)
                size, name = parse_header(line, 3)
                self.records["data-name"] = name
                if not self.acknowledge(connection, "data-header"):
                    return
                self.records["data"] = receive_exact(connection, size)
                if receive_exact(connection, 1) != b"\x00":
                    raise AssertionError("data record lacks its terminator")
                if not self.acknowledge(connection, "data-body"):
                    return

                line = receive_line(connection)
                size, name = parse_header(line, 2)
                self.records["control-name"] = name
                if not self.acknowledge(connection, "control-header"):
                    return
                self.records["control"] = receive_exact(connection, size)
                if receive_exact(connection, 1) != b"\x00":
                    raise AssertionError("control record lacks its terminator")
                self.acknowledge(connection, "control-body")
        except BaseException as caught:
            self.error = caught


def invoke(binary, arguments, input_data=None, environment=None, reject=None):
    """Run one frontend against a fresh fake receiver."""
    with FakeLpd(reject) as server:
        destination = "127.0.0.1:%d/queue" % server.port
        command = [str(binary)] + [item.replace("@DEST@", destination)
                                   for item in arguments]
        process = subprocess.run(
            command,
            input=input_data,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            check=False,
        )
    return process, server.records


def require(condition, message):
    """Raise a useful fixture failure when condition is false."""
    if not condition:
        raise AssertionError(message)


def main():
    """Run the complete host protocol matrix."""
    if len(sys.argv) != 3:
        raise SystemExit("usage: fake-lpd-test.py LP LPR")

    lp = pathlib.Path(sys.argv[1])
    lpr = pathlib.Path(sys.argv[2])
    with tempfile.TemporaryDirectory(prefix="zedbsd-lpd-test.") as directory:
        pdf = pathlib.Path(directory) / "job name.pdf"
        bad = pathlib.Path(directory) / "not-pdf"
        pdf.write_bytes(PDF)
        bad.write_bytes(b"plain text\n")

        process, records = invoke(
            lp,
            ["-d", "@DEST@", "-m", "-n", "2", "-t", "test title", str(pdf)],
        )
        require(process.returncode == 0, process.stderr.decode())
        require(process.stdout.startswith(b"request id is queue-"),
                "lp did not print a request id")
        require(records["data"] == PDF, "lp changed the PDF payload")
        control = records["control"]
        data_name = records["data-name"]
        require(control.count(b"l" + data_name + b"\n") == 2,
                "lp did not encode both copies")
        require(b"M" in control and b"Jtest_title\n" in control,
                "lp mail/title control fields are missing")
        require(b"Njob_name.pdf\n" in control,
                "lp did not sanitize the source name")

        process, records = invoke(
            lpr,
            ["-P", "@DEST@", "-J", "stdin title", "-"],
            input_data=PDF,
        )
        require(process.returncode == 0, process.stderr.decode())
        require(process.stdout == b"", "lpr unexpectedly printed a request id")
        require(records["data"] == PDF, "lpr changed stdin PDF data")
        require(b"Jstdin_title\n" in records["control"],
                "lpr title was not encoded")

        for stage in ACK_STAGES:
            process, _ = invoke(lp, ["-d", "@DEST@", str(pdf)], reject=stage)
            require(process.returncode != 0,
                    "lp accepted a refusal at %s" % stage)

        environment = os.environ.copy()
        with FakeLpd() as server:
            environment["LPDEST"] = "127.0.0.1:%d/queue" % server.port
            environment["PRINTER"] = "invalid-printer-value"
            process = subprocess.run(
                [str(lp), "-s", str(pdf)],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                env=environment,
                check=False,
            )
        require(process.returncode == 0, process.stderr.decode())
        require(server.records["data"] == PDF,
                "LPDEST did not take precedence over PRINTER")

        invalid_cases = (
            (["-d", "missing-queue", str(pdf)], None),
            (["-d", "127.0.0.1:9/queue", str(bad)], None),
            (["-d", "127.0.0.1:9/queue", "-w", str(pdf)], None),
            (["-d", "127.0.0.1:9/queue", "-o", "media=a4", str(pdf)], None),
        )
        for arguments, input_data in invalid_cases:
            process = subprocess.run(
                [str(lp)] + arguments,
                input=input_data,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                check=False,
            )
            require(process.returncode != 0,
                    "lp accepted invalid arguments/input: %r" % arguments)

    print("LPD-T001 direct lp/lpr protocol and refusal matrix: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
