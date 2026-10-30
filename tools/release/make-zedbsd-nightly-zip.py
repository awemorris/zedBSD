#!/usr/bin/env python3
# Add a zedBSD disk image to the zedbsd-nightly base archive.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# The base archive holds the Windows QEMU, its libraries, firmware and
# licenses under zedbsd-nightly/. The output is the base archive with every
# entry copied unchanged plus zedbsd-nightly/data/hdd-image.img, which boot.ps1
# starts.

import argparse
import hashlib
import os
import re
import shutil
import sys
import zipfile

ROOT = "zedbsd-nightly/"
IMAGE = ROOT + "data/hdd-image.img"
REQUIRED = (ROOT + "boot.bat", ROOT + "boot.ps1", ROOT + "qemu-system-x86_64.exe",
            ROOT + "libvirglrenderer-1.dll", ROOT + "data/edk2-x86_64-code.fd",
            ROOT + "data/ovmf-vars.fd", ROOT + "THIRD-PARTY.txt")
DRAFT_MARK = b"not yet confirmed; draft package"


def fail(message):
    print("make-zedbsd-nightly-zip: " + message, file=sys.stderr)
    sys.exit(1)


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", required=True, help="the zedbsd-nightly base archive")
    parser.add_argument("--sha256", required=True, help="pinned SHA-256 of the base archive")
    parser.add_argument("--image", required=True, help="the amd64 hdd-image.img to add")
    parser.add_argument("--output", required=True, help="zedbsd-nightly.zip to write")
    parser.add_argument("--allow-draft", action="store_true",
                        help="accept a base archive whose fork commits are unconfirmed")
    args = parser.parse_args()

    if not re.fullmatch(r"[0-9a-f]{64}", args.sha256):
        fail("the pinned SHA-256 is not set")
    if sha256_file(args.base) != args.sha256:
        fail("%s does not match the pinned SHA-256" % args.base)
    if not os.path.isfile(args.image):
        fail("missing disk image %s" % args.image)

    with zipfile.ZipFile(args.base) as base:
        names = base.namelist()
        if len(set(names)) != len(names):
            fail("the base archive has duplicate members")
        for name in names:
            if (not name.startswith(ROOT) or name.startswith("/") or "\\" in name
                    or ".." in name.split("/")):
                fail("unexpected member %s" % name)
        for name in REQUIRED:
            if name not in names:
                fail("the base archive lacks %s" % name)
        if IMAGE in names:
            fail("the base archive already contains %s" % IMAGE)
        if DRAFT_MARK in base.read(ROOT + "THIRD-PARTY.txt") and not args.allow_draft:
            fail("the base archive is a draft (fork commits unconfirmed)")
        if base.testzip() is not None:
            fail("the base archive is corrupt")

    output = os.path.abspath(args.output)
    os.makedirs(os.path.dirname(output), exist_ok=True)
    temporary = output + ".tmp"
    try:
        shutil.copyfile(args.base, temporary)
        with zipfile.ZipFile(temporary, "a") as out:
            info = zipfile.ZipInfo.from_file(args.image, IMAGE)
            epoch = os.environ.get("SOURCE_DATE_EPOCH")
            if epoch:
                import time
                info.date_time = time.gmtime(int(epoch))[:6]
            info.create_system = 3
            info.external_attr = 0o100644 << 16
            info.compress_type = zipfile.ZIP_DEFLATED
            with open(args.image, "rb") as source, out.open(info, "w", force_zip64=True) as sink:
                shutil.copyfileobj(source, sink, 1 << 20)
        os.replace(temporary, output)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)

    print("%s  %s  %d bytes" % (sha256_file(output), args.output, os.path.getsize(output)))


if __name__ == "__main__":
    main()
