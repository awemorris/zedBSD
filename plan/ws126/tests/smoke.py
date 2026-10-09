#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib
"""P2 of WS126: the representative standard modules work on the guest.

Run on the guest as `python3 smoke.py`.  Each module's check prints
`NAME: ok` or `FAIL: NAME: why`; the last line is `smoke: PASS` or
`smoke: FAIL`.  Temporary files go to a directory of the run under the
system's temporary directory and are left there.
"""

import sys
import traceback

checks = []


def check(function):
    """Registers one module's check."""
    checks.append(function)
    return function


@check
def os_module():
    """os: the process, the environment and a directory listing."""
    import os
    assert os.getpid() > 0
    assert os.path.isdir("/usr/lib/python3.14")
    assert "os.py" in os.listdir("/usr/lib/python3.14")


@check
def sys_module():
    """sys: the version and the platform."""
    assert sys.version_info[:2] == (3, 14)
    assert sys.platform.startswith("zedbsd")


@check
def re_module():
    """re: a pattern with groups."""
    import re
    match = re.fullmatch(r"(\w+)-(\d+)", "kei-2026")
    assert match.group(1) == "kei" and match.group(2) == "2026"


@check
def json_module():
    """json: a round trip through the C accelerator."""
    import json
    import _json  # noqa: F401  the accelerator must be there
    value = {"a": [1, 2.5, None, True], "b": "日本語"}
    assert json.loads(json.dumps(value)) == value


@check
def datetime_module():
    """datetime: arithmetic and formatting."""
    import datetime
    day = datetime.date(2026, 10, 9) + datetime.timedelta(days=30)
    assert day.isoformat() == "2026-11-08"
    assert datetime.datetime.now().year >= 2026


@check
def pathlib_module():
    """pathlib: writing and reading a file."""
    import pathlib
    import tempfile
    directory = pathlib.Path(tempfile.mkdtemp(prefix="smoke."))
    path = directory / "a.txt"
    path.write_text("zedBSD\n", encoding="utf-8")
    assert path.read_text(encoding="utf-8") == "zedBSD\n"


@check
def subprocess_module():
    """subprocess: /bin/echo's output and status."""
    import subprocess
    completed = subprocess.run(["/bin/echo", "hello"], capture_output=True, check=True)
    assert completed.stdout == b"hello\n"


@check
def threading_module():
    """threading: two threads and a lock."""
    import threading
    total = [0]
    lock = threading.Lock()

    def add():
        for _ in range(10000):
            with lock:
                total[0] += 1

    threads = [threading.Thread(target=add) for _ in range(2)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()
    assert total[0] == 20000


@check
def socket_module():
    """socket and select: a loopback TCP exchange."""
    import select
    import socket
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.bind(("127.0.0.1", 0))
    listener.listen(1)
    client = socket.create_connection(listener.getsockname(), timeout=10)
    server, _ = listener.accept()
    client.sendall(b"ping")
    readable, _, _ = select.select([server], [], [], 10)
    assert readable == [server]
    assert server.recv(4) == b"ping"
    for item in (client, server, listener):
        item.close()


@check
def asyncio_module():
    """asyncio: a loopback echo server and client."""
    import asyncio

    async def run():
        async def echo(reader, writer):
            writer.write(await reader.readline())
            await writer.drain()
            writer.close()

        server = await asyncio.start_server(echo, "127.0.0.1", 0)
        port = server.sockets[0].getsockname()[1]
        reader, writer = await asyncio.open_connection("127.0.0.1", port)
        writer.write(b"asyncio\n")
        line = await reader.readline()
        writer.close()
        server.close()
        await server.wait_closed()
        return line

    assert asyncio.run(run()) == b"asyncio\n"


@check
def zlib_module():
    """zlib: compress and decompress."""
    import zlib
    data = b"zedBSD " * 1000
    assert zlib.decompress(zlib.compress(data)) == data


@check
def hashlib_module():
    """hashlib: sha256 and blake2b."""
    import hashlib
    assert hashlib.sha256(b"abc").hexdigest() == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
    assert len(hashlib.blake2b(b"abc").digest()) == 64


@check
def unicodedata_module():
    """unicodedata: a name and a normalization."""
    import unicodedata
    assert unicodedata.name("あ") == "HIRAGANA LETTER A"
    assert unicodedata.normalize("NFC", "が") == "が"


@check
def decimal_module():
    """decimal: the C implementation and exact arithmetic."""
    import decimal
    assert decimal.__name__ == "decimal"
    import _decimal  # noqa: F401  the C implementation must be there
    assert decimal.Decimal("0.1") + decimal.Decimal("0.2") == decimal.Decimal("0.3")


@check
def struct_math_module():
    """struct and math."""
    import math
    import struct
    assert struct.unpack("<I", struct.pack("<I", 0x12345678))[0] == 0x12345678
    assert math.isclose(math.sqrt(2.0) ** 2, 2.0)
    assert math.isclose(math.lgamma(5.0), math.log(24.0))


@check
def random_secrets_module():
    """random and secrets."""
    import random
    import secrets
    generator = random.Random(1)
    assert 0 <= generator.random() < 1
    assert len(secrets.token_bytes(16)) == 16


@check
def tempfile_shutil_module():
    """tempfile and shutil: a copy of a tree."""
    import os
    import shutil
    import tempfile
    source = tempfile.mkdtemp(prefix="smoke-src.")
    with open(os.path.join(source, "f"), "w", encoding="ascii") as stream:
        stream.write("x")
    target = source + "-copy"
    shutil.copytree(source, target)
    assert os.path.isfile(os.path.join(target, "f"))


@check
def locale_module():
    """locale: UTF-8 in and out."""
    import locale
    locale.setlocale(locale.LC_ALL, "")
    assert sys.getfilesystemencoding() == "utf-8"
    encoded = "日本語".encode(locale.getpreferredencoding(False) or "utf-8")
    assert encoded.decode("utf-8") == "日本語"


def square(value):
    """A worker of the multiprocessing check."""
    return value * value


@check
def multiprocessing_module():
    """multiprocessing: a pool of forked workers."""
    import multiprocessing
    context = multiprocessing.get_context("fork")
    with context.Pool(2) as pool:
        assert pool.map(square, range(5)) == [0, 1, 4, 9, 16]


def main():
    """Runs every check and prints the verdict."""
    failed = 0
    for function in checks:
        try:
            function()
            print(f"{function.__name__}: ok", flush=True)
        except Exception:
            failed += 1
            reason = traceback.format_exc().strip().splitlines()[-1]
            print(f"FAIL: {function.__name__}: {reason}", flush=True)
    print("smoke: FAIL" if failed else "smoke: PASS", flush=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
