#!/usr/bin/env python3
# Build the image-less zedbsd-nightly base archive from the original WINQ-EMU
# package.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The base archive is the original package (Kei-nightly.zip, the project's
# earlier name) with its top directory renamed to zedbsd-nightly/, the two
# unused QEMU executables removed, the NVMe disk's serial in boot.ps1 named
# after the system, the corrected README.txt, THIRD-PARTY.txt and the
# license texts under LICENSES/. Every input is pinned by SHA-256
# (tools/release/zedbsd-nightly/inputs.txt), and the output is deterministic for
# the same inputs: entries are sorted, original entries keep their original
# timestamps, and added entries use ADDED_TIME.

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

ORIGINAL_SHA256 = "2a66519e0f9153b50600cb5f9703d084e6536f4e51f0eb3c3638fa0e976a3ca0"
# The original's top directory, fixed by its SHA-256, and the base's.
ORIGINAL_ROOT = "Kei-nightly/"
ROOT = "zedbsd-nightly/"
REMOVED = (ORIGINAL_ROOT + "qemu-built.exe", ORIGINAL_ROOT + "qemu-system-x86_64w.exe")
FORBIDDEN = (ORIGINAL_ROOT + "data/hdd-image.img",)
# The NVMe disk's serial in the original boot.ps1, and the base's.
ORIGINAL_SERIAL = b"nvme,serial=kei-boot,"
SERIAL = b"nvme,serial=zedbsd-boot,"
ADDED_TIME = (2026, 9, 29, 0, 0, 0)

# Bundled DLL -> MSYS2 package that must contain a byte-identical copy.
MSYS2_DLLS = {
    "SDL2.dll": "SDL2-2.32.10-1",
    "libbz2-1.dll": "bzip2-1.0.8-3",
    "libepoxy-0.dll": "libepoxy-1.5.10-7",
    "libfdt-1.dll": "dtc-1.7.2-3",
    "libffi-8.dll": "libffi-3.5.2-1",
    "libgcc_s_seh-1.dll": "gcc-libs-15.2.0-14",
    "libgio-2.0-0.dll": "glib2-2.88.0-1",
    "libglib-2.0-0.dll": "glib2-2.88.0-1",
    "libgmodule-2.0-0.dll": "glib2-2.88.0-1",
    "libgobject-2.0-0.dll": "glib2-2.88.0-1",
    "libiconv-2.dll": "libiconv-1.19-1",
    "libintl-8.dll": "gettext-runtime-1.0-1",
    "libncursesw6.dll": "ncurses-6.6-4",
    "libpcre2-8-0.dll": "pcre2-10.47-1",
    "libpixman-1-0.dll": "pixman-0.46.4-1",
    "libslirp-0.dll": "libslirp-4.9.1-2",
    "libwinpthread-1.dll": "libwinpthread-14.0.0.r14.g4761eabdd-1",
    "libzstd.dll": "zstd-1.5.7-1",
    "zlib1.dll": "zlib-1.3.2-2",
}


def fail(message):
    print("make-zedbsd-nightly-base: " + message, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_table(path, columns):
    rows = []
    with open(path, encoding="utf-8") as stream:
        for number, line in enumerate(stream, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split()
            if len(fields) != columns:
                fail("%s:%d: expected %d fields" % (path, number, columns))
            rows.append(fields)
    return rows


def fetch_inputs(table, cache):
    """Return {name: path} for every pinned input, downloading when absent."""
    os.makedirs(cache, exist_ok=True)
    paths = {}
    for name, digest, url in table:
        if not re.fullmatch(r"[0-9a-f]{64}", digest):
            fail("bad digest for %s" % name)
        path = os.path.join(cache, name)
        if not os.path.exists(path):
            print("fetch " + url)
            handle, temporary = tempfile.mkstemp(dir=cache, prefix="." + name + ".")
            try:
                with os.fdopen(handle, "wb") as out:
                    with urllib.request.urlopen(url, timeout=300) as response:
                        shutil.copyfileobj(response, out)
                if sha256_file(temporary) != digest:
                    fail("downloaded %s: SHA-256 mismatch" % name)
                os.replace(temporary, path)
            finally:
                if os.path.exists(temporary):
                    os.unlink(temporary)
        if sha256_file(path) != digest:
            fail("cached %s: SHA-256 mismatch" % path)
        paths[name] = path
    return paths


def extract_members(archive, members, destination):
    """Extract the named members of a tar archive (any compression)."""
    for member in members:
        if member.startswith("/") or ".." in member.split("/"):
            fail("unsafe member %s" % member)
    subprocess.run(["tar", "-xf", archive, "-C", destination, "--"] + sorted(members),
                   check=True)


def crlf(data):
    return data.replace(b"\r\n", b"\n").replace(b"\n", b"\r\n")


def entry(name, date_time, directory):
    info = zipfile.ZipInfo(name, date_time)
    info.create_system = 3
    if directory:
        info.external_attr = (0o40755 << 16) | 0x10
        info.compress_type = zipfile.ZIP_STORED
    else:
        info.external_attr = 0o100644 << 16
        info.compress_type = zipfile.ZIP_DEFLATED
    return info


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    data = os.path.join(here, "zedbsd-nightly")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--original", required=True, help="the original WINQ-EMU package (Kei-nightly.zip)")
    parser.add_argument("--cache", required=True, help="directory for the pinned inputs")
    parser.add_argument("--output", required=True, help="base archive to write")
    parser.add_argument("--qemu-commit", help="commit of the QEMU fork the binaries were built from")
    parser.add_argument("--virglrenderer-commit", help="commit of the virglrenderer fork")
    parser.add_argument("--draft", action="store_true",
                        help="allow unconfirmed fork commits (not for publication)")
    args = parser.parse_args()

    commits = {}
    for key, value in (("@QEMU_COMMIT@", args.qemu_commit),
                       ("@VIRGLRENDERER_COMMIT@", args.virglrenderer_commit)):
        if value:
            if not re.fullmatch(r"[0-9a-f]{40}", value):
                fail("%s must be a full 40-digit commit id" % key)
            commits[key] = value
        elif args.draft:
            commits[key] = "(not yet confirmed; draft package)"
        else:
            fail("the fork commits are required (or pass --draft)")

    if sha256_file(args.original) != ORIGINAL_SHA256:
        fail("the original archive does not match the pinned SHA-256")
    inputs = fetch_inputs(read_table(os.path.join(data, "inputs.txt"), 3), args.cache)
    licenses = read_table(os.path.join(data, "licenses.txt"), 3)

    output_directory = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(output_directory, exist_ok=True)
    work = tempfile.mkdtemp(prefix=".zedbsd-nightly-base.", dir=output_directory)
    try:
        # Group the members wanted from each archive: license texts and the
        # DLLs that are checked against their packages.
        wanted = {}
        for _, source, member in licenses:
            if source not in inputs:
                fail("license source %s is not a pinned input" % source)
            if member != "-":
                wanted.setdefault(source, set()).add(member)
        for dll, package in MSYS2_DLLS.items():
            source = "mingw-w64-ucrt-x86_64-%s-any.pkg.tar.zst" % package
            if source not in inputs:
                fail("package %s is not a pinned input" % source)
            wanted.setdefault(source, set()).add("ucrt64/bin/" + dll)
        for source, members in sorted(wanted.items()):
            destination = os.path.join(work, source)
            os.makedirs(destination)
            extract_members(inputs[source], members, destination)

        added = {}
        for target, source, member in licenses:
            if member == "-":
                path = inputs[source]
            else:
                path = os.path.join(work, source, member)
                if os.path.islink(path) or not os.path.isfile(path):
                    fail("%s in %s is not a regular file" % (member, source))
            with open(path, "rb") as stream:
                added[ROOT + "LICENSES/" + target] = stream.read()
        with open(os.path.join(data, "winq-emu-license.txt"), "rb") as stream:
            added[ROOT + "LICENSES/winq-emu/README.txt"] = crlf(stream.read())
        with open(os.path.join(data, "README.txt"), "rb") as stream:
            readme = crlf(stream.read())
        with open(os.path.join(data, "THIRD-PARTY.txt"), "rb") as stream:
            notice = stream.read().decode("utf-8")
        for key, value in commits.items():
            if key not in notice:
                fail("THIRD-PARTY.txt lacks %s" % key)
            notice = notice.replace(key, value)
        added[ROOT + "THIRD-PARTY.txt"] = crlf(notice.encode("utf-8"))

        with zipfile.ZipFile(args.original) as original:
            infos = {info.filename: info for info in original.infolist()}
            for name in infos:
                if not name.startswith(ORIGINAL_ROOT) or ".." in name.split("/") or "\\" in name:
                    fail("unexpected member %s" % name)
            for name in REMOVED + (ORIGINAL_ROOT + "README.txt", ORIGINAL_ROOT + "boot.ps1"):
                if name not in infos:
                    fail("the original lacks %s" % name)
            for name in FORBIDDEN:
                if name in infos:
                    fail("the original contains %s" % name)
            for dll, package in MSYS2_DLLS.items():
                source = "mingw-w64-ucrt-x86_64-%s-any.pkg.tar.zst" % package
                with open(os.path.join(work, source, "ucrt64/bin", dll), "rb") as stream:
                    if original.read(ORIGINAL_ROOT + dll) != stream.read():
                        fail("%s differs from %s" % (dll, package))

            members = {}
            for name, info in infos.items():
                if name in REMOVED:
                    continue
                renamed = ROOT + name[len(ORIGINAL_ROOT):]
                if name == ORIGINAL_ROOT + "README.txt":
                    members[renamed] = (info.date_time, readme)
                elif name == ORIGINAL_ROOT + "boot.ps1":
                    script = original.read(name)
                    if script.count(ORIGINAL_SERIAL) != 1:
                        fail("boot.ps1 does not name the disk's serial once")
                    members[renamed] = (info.date_time, script.replace(ORIGINAL_SERIAL, SERIAL))
                elif info.is_dir():
                    members[renamed] = (info.date_time, None)
                else:
                    members[renamed] = (info.date_time, original.read(name))
            for name, content in added.items():
                if name in members:
                    fail("%s already exists in the original" % name)
                members[name] = (ADDED_TIME, content)
                parent = name.rsplit("/", 1)[0] + "/"
                while parent != ROOT and parent not in members:
                    members[parent] = (ADDED_TIME, None)
                    parent = parent.rstrip("/").rsplit("/", 1)[0] + "/"

        temporary = args.output + ".tmp"
        with zipfile.ZipFile(temporary, "w") as out:
            for name in sorted(members):
                date_time, content = members[name]
                info = entry(name, date_time, content is None)
                out.writestr(info, content if content is not None else b"",
                             compresslevel=9 if content is not None else None)
        os.replace(temporary, args.output)
    finally:
        shutil.rmtree(work)

    print("%s  %s  %d bytes" % (sha256_file(args.output), args.output,
                                os.path.getsize(args.output)))


if __name__ == "__main__":
    main()
