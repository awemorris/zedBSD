# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Writes the fixtures of plan/tools/installer/host/install-admin-unpack-test.sh into OUT: one tarball and one index a case.
import gzip, hashlib, io, sys, tarfile
out = sys.argv[1]

def entry(name, kind, mode=0o644, data=b"", link=""):
	info = tarfile.TarInfo(name)
	info.type = kind
	info.mode = mode
	info.uid = info.gid = 0
	info.uname, info.gname = "root", "wheel"
	info.size = len(data) if kind == tarfile.REGTYPE else 0
	info.linkname = link
	return info, data

def tarball(entries, fmt=tarfile.USTAR_FORMAT):
	raw = io.BytesIO()
	with tarfile.open(fileobj=raw, mode="w", format=fmt) as archive:
		for info, data in entries:
			archive.addfile(info, io.BytesIO(data) if info.type == tarfile.REGTYPE else None)
	return raw.getvalue()

def gz(data):
	raw = io.BytesIO()
	with gzip.GzipFile(filename="", mode="wb", fileobj=raw, mtime=0) as stream:
		stream.write(data)
	return raw.getvalue()

def write_case(case, blob, extra="", sha=None, size=None):
	open(f"{out}/{case}.tar.gz", "wb").write(blob)
	digest = sha or hashlib.sha256(blob).hexdigest()
	with open(f"{out}/{case}.index", "w") as index:
		index.write("format\t1\n")
		index.write(f"unit\t{case}\tCase\tbase\t1\t{size if size is not None else len(blob)}\t0\t0\t{digest}\t-\n")
		index.write(extra)

good = [entry("usr", tarfile.DIRTYPE, 0o755), entry("usr/bin", tarfile.DIRTYPE, 0o755),
	entry("usr/bin/tool", tarfile.REGTYPE, 0o755, b"#!/bin/sh\necho tool\n" * 100),
	entry("usr/bin/su", tarfile.REGTYPE, 0o4755, b"su"),
	entry("usr/bin/link", tarfile.SYMTYPE, 0o777, link="tool"),
	entry("tmp", tarfile.DIRTYPE, 0o1777)]
write_case("good", gz(tarball(good)), "setid\tgood\t/usr/bin/su\t4755\n")
write_case("setid", gz(tarball(good)))
write_case("absolute", gz(tarball([entry("/etc/x", tarfile.REGTYPE, 0o644, b"x")])))
write_case("dotdot", gz(tarball([entry("usr", tarfile.DIRTYPE, 0o755), entry("usr/../x", tarfile.REGTYPE, 0o644, b"x")])))
write_case("escape", gz(tarball([entry("d", tarfile.DIRTYPE, 0o755), entry("d/l", tarfile.SYMTYPE, 0o777, link=f"{out}/outside"),
	entry("d/l/x", tarfile.REGTYPE, 0o644, b"x")])))
write_case("hardlink", gz(tarball([entry("a", tarfile.REGTYPE, 0o644, b"a"), entry("b", tarfile.LNKTYPE, 0o644, link="a")])))
write_case("pax", gz(tarball([entry("p" * 150 + "/" + "q" * 120, tarfile.REGTYPE, 0o644, b"x")], tarfile.PAX_FORMAT)))
broken = bytearray(tarball(good))
broken[10] ^= 1
write_case("checksum", gz(bytes(broken)), "setid\tcheckum\t/usr/bin/su\t4755\n".replace("checkum", "checksum"))
write_case("digest", gz(tarball(good)), sha="0" * 64)
write_case("size", gz(tarball(good)), size=len(gz(tarball(good))) - 1)
crc = bytearray(gz(tarball(good)))
crc[-8] ^= 0xff
write_case("crc", bytes(crc), "setid\tcrc\t/usr/bin/su\t4755\n")
with open(f"{out}/deps.index", "w") as index:
	index.write("format\t1\n")
	for name, deps in (("a", "b"), ("b", "c"), ("c", "-"), ("x", "y"), ("y", "x"), ("d", "c a")):
		index.write(f"unit\t{name}\t{name}\tpackage\t1\t1\t1\t1\t{'0' * 64}\t{deps}\n")
with open(f"{out}/bad-format.index", "w") as index:
	index.write("format\t2\n")
with open(f"{out}/unknown-dep.index", "w") as index:
	index.write("format\t1\nunit\ta\ta\tbase\t1\t1\t1\t1\t" + "0" * 64 + "\tnone\n")
with open(f"{out}/unknown-record.index", "w") as index:
	index.write("format\t1\nfile\ta\n")
print("fixtures", out)
