# The direct checks of zedBSD's sftp-server for interop-host.sh: the protocol on its standard streams (no ssh), with
# the expected bytes built here from draft-ietf-secsh-filexfer-02 (3 packets, 7 status codes, 6.x requests) and
# OpenSSH PROTOCOL 4.1 (SYMLINK's order); and the malformed-input runs.
#   python3 -I sftp-host.py raw SERVER DIR              the protocol checks, in DIR (empty, the test's)
#   python3 -I sftp-host.py fuzz-structured SERVER DIR N  N runs of requests whose paths are only a few short names
#                                                           without "/" (so the real file system is touched only in DIR)
#   python3 -I sftp-host.py fuzz-random SERVER DIR N      N runs of random bytes, for a server built with
#                                                           sftp-fs-stub.c (no real file system call at all)
# Prints "ok ..." lines or "FAIL ..." lines; exits 0 when nothing failed.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os, random, struct, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

FXP = dict(INIT=1, VERSION=2, OPEN=3, CLOSE=4, READ=5, WRITE=6, LSTAT=7, FSTAT=8, SETSTAT=9, FSETSTAT=10, OPENDIR=11,
           READDIR=12, REMOVE=13, MKDIR=14, RMDIR=15, REALPATH=16, STAT=17, RENAME=18, READLINK=19, SYMLINK=20,
           STATUS=101, HANDLE=102, DATA=103, NAME=104, ATTRS=105, EXTENDED=200)
FX_OK, FX_EOF, FX_NO_SUCH_FILE, FX_PERMISSION_DENIED, FX_FAILURE, FX_BAD_MESSAGE, FX_OP_UNSUPPORTED = 0, 1, 2, 3, 4, 5, 8
FXF_READ, FXF_WRITE, FXF_CREAT, FXF_TRUNC = 1, 2, 8, 16

def u32(v): return struct.pack(">I", v & 0xffffffff)
def u64(v): return struct.pack(">Q", v)
def string(b):
    if isinstance(b, str): b = b.encode()
    return u32(len(b)) + b
def packet(kind, body): return u32(len(body) + 1) + bytes([kind]) + body
def request(kind, rid, body=b""): return packet(kind, u32(rid) + body)
def no_attrs(): return u32(0)

def run(server, cwd, data, timeout=20):
    env = {"PATH": "/usr/bin:/bin", "ASAN_OPTIONS": "detect_leaks=1:exitcode=99", "UBSAN_OPTIONS": "halt_on_error=1:exitcode=99"}
    p = subprocess.run([server], input=data, cwd=cwd, capture_output=True, timeout=timeout, env=env)
    return p.returncode, p.stdout, p.stderr

def replies(data):
    out = []
    while len(data) >= 5:
        n = struct.unpack(">I", data[:4])[0]
        out.append(data[4:4 + n])
        data = data[4 + n:]
    return out

def status_of(reply):
    if reply[0] != FXP["STATUS"]: return None
    return struct.unpack(">I", reply[1:5])[0], struct.unpack(">I", reply[5:9])[0]

failures = 0
def check(name, condition):
    global failures
    if condition:
        print("ok " + name)
    else:
        failures += 1
        print("FAIL " + name)

def raw(server, d):
    init = packet(FXP["INIT"], u32(3))
    # VERSION: uint32 length 5, byte 2, uint32 version 3, no extension pairs (draft 4).
    code, out, err = run(server, d, init)
    check("INIT gives VERSION 3 with no extension (00 00 00 05 02 00 00 00 03)", code == 0 and out == bytes.fromhex("000000050200000003"))
    code, out, err = run(server, d, request(FXP["REALPATH"], 7, string(".")))
    check("a request before INIT ends the server with no answer", code == 1 and out == b"")

    open(os.path.join(d, "noperm"), "w").write("x")
    os.chmod(os.path.join(d, "noperm"), 0)
    open(os.path.join(d, "data"), "wb").write(b"0123456789")
    seq = [request(FXP["OPEN"], 1, string("nofile") + u32(FXF_READ) + no_attrs()),
           request(FXP["OPEN"], 2, string("noperm") + u32(FXF_READ) + no_attrs()),
           request(FXP["SYMLINK"], 3, string("data") + string("link")),
           request(FXP["READLINK"], 4, string("link")),
           request(FXP["RENAME"], 5, string("data") + string("noperm")),
           request(FXP["EXTENDED"], 6, string("statvfs@openssh.com") + string(".")),
           packet(FXP["READ"], u32(7) + u32(4)),
           request(FXP["CLOSE"], 8, string(u32(64))),
           request(FXP["OPEN"], 9, string("data") + u32(FXF_READ) + no_attrs()),
           request(FXP["READ"], 10, string(u32(0)) + u64(4) + u32(3)),
           request(FXP["READ"], 11, string(u32(0)) + u64(10) + u32(3)),
           request(FXP["REALPATH"], 12, string(".")),
           request(FXP["STAT"], 13, string("data")),
           request(FXP["RMDIR"], 14, string("nodir")),
           request(FXP["REALPATH"], 15, string("not-yet")),
           request(FXP["REALPATH"], 16, string("no-dir/not-yet"))]
    code, out, err = run(server, d, init + b"".join(seq))
    r = replies(out)
    check("the server answers every request (%d)" % len(r), code == 0 and len(r) == 1 + len(seq))
    if len(r) != 1 + len(seq):
        return
    check("OPEN of a missing file is NO_SUCH_FILE (2)", status_of(r[1]) == (1, FX_NO_SUCH_FILE))
    check("OPEN of a file without permission is PERMISSION_DENIED (3)", status_of(r[2]) == (2, FX_PERMISSION_DENIED))
    check("SYMLINK (OpenSSH's order: target, then the new link) is OK", status_of(r[3]) == (3, FX_OK))
    check("the link points the right way", os.readlink(os.path.join(d, "link")) == "data")
    # NAME: byte 104, id, count 1, filename, longname, attrs flags 0 (draft 7).
    check("READLINK gives the link's target", r[4] == bytes([104]) + u32(4) + u32(1) + string("data") + string("data") + u32(0))
    check("RENAME onto an existing name is FAILURE (4)", status_of(r[5]) == (5, FX_FAILURE))
    check("an extension is OP_UNSUPPORTED (8)", status_of(r[6]) == (6, FX_OP_UNSUPPORTED))
    check("a malformed READ is BAD_MESSAGE (5) with its id", status_of(r[7]) == (7, FX_BAD_MESSAGE))
    check("a handle outside the table is FAILURE", status_of(r[8]) == (8, FX_FAILURE))
    check("OPEN gives the 4-byte handle 0", r[9] == bytes([102]) + u32(9) + string(u32(0)))
    check("READ at 4 of 3 bytes gives 456", r[10] == bytes([103]) + u32(10) + string("456"))
    check("READ at the end is EOF (1)", status_of(r[11]) == (11, FX_EOF))
    real = os.path.realpath(d)
    check("REALPATH of . is the working directory", r[12] == bytes([104]) + u32(12) + u32(1) + string(real) + string(real) + u32(0))
    # ATTRS: flags 0x0f, size 10, uid, gid, permissions, atime, mtime (draft 5).
    st = os.stat(os.path.join(d, "data"))
    want = bytes([105]) + u32(13) + u32(0x0f) + u64(10) + u32(st.st_uid) + u32(st.st_gid) + u32(st.st_mode) + u32(int(st.st_atime)) + u32(int(st.st_mtime))
    check("STAT gives the attributes", r[13] == want)
    check("RMDIR of a missing directory is NO_SUCH_FILE", status_of(r[14]) == (14, FX_NO_SUCH_FILE))
    want = os.path.join(real, "not-yet")
    check("REALPATH of a name not yet there is its absolute form", r[15] == bytes([104]) + u32(15) + u32(1) + string(want) + string(want) + u32(0))
    check("REALPATH below a directory that is not there is NO_SUCH_FILE", status_of(r[16]) == (16, FX_NO_SUCH_FILE))

    # The link is gone after REMOVE, and its target stays.
    code, out, err = run(server, d, init + request(FXP["REMOVE"], 1, string("link")) + request(FXP["LSTAT"], 2, string("link")))
    r = replies(out)
    check("REMOVE of the link is OK, then LSTAT is NO_SUCH_FILE", len(r) == 3 and status_of(r[1]) == (1, FX_OK) and status_of(r[2]) == (2, FX_NO_SUCH_FILE))
    check("the link's target stays", os.path.exists(os.path.join(d, "data")))

    # Attributes, truncation, modes, READ's bound, READDIR's form, LSTAT of a link, an empty REALPATH.
    big = os.path.join(d, "big")
    open(big, "wb").write(b"z" * 70000)
    os.utime(big, (1000, 2000))
    os.symlink("big", os.path.join(d, "biglink"))
    os.mkdir(os.path.join(d, "dir"))
    for n in ("x", "y", "z"):
        open(os.path.join(d, "dir", n), "w").write(n)
    seq = [request(FXP["STAT"], 1, string("big")),
           request(FXP["LSTAT"], 2, string("biglink")),
           request(FXP["OPEN"], 3, string("big") + u32(FXF_READ) + no_attrs()),
           request(FXP["READ"], 4, string(u32(0)) + u64(0) + u32(65537)),
           request(FXP["CLOSE"], 5, string(u32(0) + b"\0")),
           request(FXP["OPEN"], 6, string("new") + u32(FXF_WRITE | FXF_CREAT) + u32(4) + u32(0o600)),
           request(FXP["FSETSTAT"], 7, string(u32(1)) + u32(4) + u32(0o640)),
           request(FXP["SETSTAT"], 8, string("big") + u32(1 | 4 | 8) + u64(3) + u32(0o604) + u32(1111) + u32(2222)),
           request(FXP["OPEN"], 9, string("trunc") + u32(FXF_WRITE | FXF_TRUNC) + no_attrs()),
           request(FXP["OPENDIR"], 10, string("dir")),
           request(FXP["READDIR"], 11, string(u32(3))),
           request(FXP["READDIR"], 12, string(u32(3))),
           request(FXP["REALPATH"], 13, string("")),
           request(FXP["OPEN"], 14, string("new2") + u32(FXF_WRITE | FXF_CREAT) + u32(4) + u32(0o600))]
    open(os.path.join(d, "trunc"), "w").write("not empty")
    code, out, err = run(server, d, init + b"".join(seq))
    r = replies(out)
    check("the second run answers every request (%d)" % len(r), len(r) == 1 + len(seq))
    if len(r) == 1 + len(seq):
        check("STAT gives the modification time apart from the access time", r[1][-8:] == u32(1000) + u32(2000))
        check("LSTAT of a link gives a link's type bits", (struct.unpack(">I", r[2][1 + 4 + 4 + 8 + 4 + 4:1 + 4 + 4 + 8 + 4 + 4 + 4])[0] & 0o170000) == 0o120000)
        check("READ of 65537 bytes gives at most 65536", r[4][0] == FXP["DATA"] and r[4][5:9] == u32(65536))
        check("a 5-byte handle is refused", status_of(r[5]) == (5, FX_FAILURE))
        check("OPEN with CREAT gives the new file the asked permissions", (os.stat(os.path.join(d, "new2")).st_mode & 0o7777) == 0o600)
        check("FSETSTAT sets the permissions", status_of(r[7]) == (7, FX_OK) and (os.stat(os.path.join(d, "new")).st_mode & 0o7777) == 0o640)
        st = os.stat(big)
        check("SETSTAT sets the size, the permissions and both times", status_of(r[8]) == (8, FX_OK) and st.st_size == 3 and (st.st_mode & 0o7777) == 0o604 and int(st.st_atime) == 1111 and int(st.st_mtime) == 2222)
        check("OPEN with TRUNC empties the file", os.path.getsize(os.path.join(d, "trunc")) == 0)
        count = struct.unpack(">I", r[11][5:9])[0] if r[11][0] == FXP["NAME"] else -1
        check("READDIR gives the five entries in one reply", count == 5)
        check("the next READDIR is EOF", status_of(r[12]) == (12, FX_EOF))
        real = os.path.realpath(d)
        check("REALPATH of an empty path is the working directory", r[13] == bytes([104]) + u32(13) + u32(1) + string(real) + string(real) + u32(0))

    # Handles left open at the end are closed (no leak report); a length of 0 ends the server; arguments are refused.
    code, out, err = run(server, d, init + request(FXP["OPENDIR"], 1, string("dir")) + request(FXP["OPEN"], 2, string("data") + u32(FXF_READ) + no_attrs()))
    check("handles left open at the end are let go", code == 0 and b"Sanitizer" not in err)
    code, out, err = run(server, d, init + u32(0))
    check("a packet length of 0 ends the server with 1", code == 1)
    p = subprocess.run([server, "-e"], input=b"", capture_output=True, timeout=10)
    check("an argument is refused", p.returncode == 1)

    # 64 handles at most: the 65th OPEN is refused, a CLOSE frees one.
    seq = [request(FXP["OPEN"], 100 + i, string("data") + u32(FXF_READ) + no_attrs()) for i in range(65)]
    seq.append(request(FXP["CLOSE"], 200, string(u32(63))))
    seq.append(request(FXP["OPEN"], 201, string("data") + u32(FXF_READ) + no_attrs()))
    code, out, err = run(server, d, init + b"".join(seq))
    r = replies(out)
    check("64 handles open", len(r) == 68 and r[64] == bytes([102]) + u32(163) + string(u32(63)))
    check("the 65th OPEN is FAILURE", len(r) == 68 and status_of(r[65]) == (164, FX_FAILURE))
    check("after a CLOSE the handle is given again", len(r) == 68 and r[67] == bytes([102]) + u32(201) + string(u32(63)))
    check("no sanitizer report", code == 0 and b"Sanitizer" not in err and b"runtime error" not in err)

NAMES = ["a", "b", "c", "ab", "bc", "ca"]
def structured(rng):
    # Paths are only NAMES: no "/", no "." or "..", so every call stays in the working directory.
    def name(): return string(rng.choice(NAMES))
    def handle(): return string(u32(rng.choice([0, 1, 2, 3, 63, 64, rng.getrandbits(32)])))
    def attrs():
        flags = rng.getrandbits(4)
        body = u32(flags)
        if flags & 1: body += u64(rng.choice([0, 1, 4096, rng.getrandbits(20)]))
        if flags & 2: body += u32(os.getuid()) + u32(os.getgid())
        if flags & 4: body += u32(rng.choice([0o644, 0o600, 0o755, 0, 0o777]))
        if flags & 8: body += u32(rng.getrandbits(31)) + u32(rng.getrandbits(31))
        return body
    out = packet(FXP["INIT"], u32(3))
    for i in range(rng.randint(1, 30)):
        kind = rng.choice(list(range(3, 21)) + [200, 2, 101, 255])
        rid = rng.getrandbits(32)
        if kind == FXP["OPEN"]: body = name() + u32(rng.getrandbits(6)) + attrs()
        elif kind in (FXP["CLOSE"], FXP["FSTAT"], FXP["READDIR"]): body = handle()
        elif kind == FXP["READ"]: body = handle() + u64(rng.choice([0, 1, 100, rng.getrandbits(63)])) + u32(rng.choice([0, 1, 65536, 70000, rng.getrandbits(32)]))
        elif kind == FXP["WRITE"]: body = handle() + u64(rng.choice([0, 5, rng.getrandbits(62)])) + string(os.urandom(rng.randint(0, 3000)))
        elif kind in (FXP["SETSTAT"], FXP["MKDIR"]): body = name() + attrs()
        elif kind == FXP["FSETSTAT"]: body = handle() + attrs()
        elif kind in (FXP["RENAME"], FXP["SYMLINK"]): body = name() + name()
        elif kind == 200: body = string("x@y") + os.urandom(rng.randint(0, 20))
        else: body = name()
        if rng.random() < 0.1:
            body = body[:rng.randint(0, len(body))]
        out += request(kind, rid, body)
    return out

def random_input(rng):
    choice = rng.random()
    if choice < 0.3:
        return os.urandom(rng.randint(0, 2000))
    out = packet(FXP["INIT"], u32(rng.choice([3, 3, 3, 1, 6, rng.getrandbits(32)])))
    for i in range(rng.randint(0, 30)):
        length = rng.choice([0, 1, 4, 5, 9, rng.randint(1, 600), 262144, 262145, rng.getrandbits(32)])
        body = bytes([rng.choice(list(range(0, 22)) + [101, 102, 200, 255])]) + os.urandom(max(0, min(length, 700) - 1))
        if rng.random() < 0.7:
            out += u32(len(body)) + body
        else:
            out += u32(length) + body
    return out

def fuzz(server, d, n, maker, seed):
    bad = []
    def one(i):
        rng = random.Random(seed * 1000003 + i)
        data = maker(rng)
        try:
            code, out, err = run(server, d, data)
        except subprocess.TimeoutExpired:
            return (i, "timeout", data)
        if code not in (0, 1) or b"Sanitizer" in err or b"runtime error" in err:
            return (i, "exit %d %s" % (code, err[-300:]), data)
        return None
    with ThreadPoolExecutor(max_workers=4) as pool:
        for r in pool.map(one, range(n)):
            if r is not None:
                bad.append(r)
    for i, what, data in bad[:3]:
        open(os.path.join(os.path.dirname(d), "fuzz-failure-%d.bin" % i), "wb").write(data)
        print("FAIL run %d: %s" % (i, what))
    check("%d runs, none crashed or stopped (%d failed)" % (n, len(bad)), not bad)

what, server, d = sys.argv[1], sys.argv[2], sys.argv[3]
if what == "raw":
    raw(server, d)
elif what == "fuzz-structured":
    fuzz(server, d, int(sys.argv[4]), structured, 1)
elif what == "fuzz-random":
    fuzz(server, d, int(sys.argv[4]), random_input, 2)
sys.exit(1 if failures else 0)
