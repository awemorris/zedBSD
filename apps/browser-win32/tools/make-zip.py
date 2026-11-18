#!/usr/bin/env python3
# zedBSD
# Copyright (C) 2026 Awe Morris
# SPDX-License-Identifier: Zlib
"""Writes the zip of OmniGrammaton for Windows, the same bytes from the same files.

    python3 -I apps/browser-win32/tools/make-zip.py DIRECTORY OUTPUT.zip

Every file under DIRECTORY goes in under the directory's own name (OmniGrammaton/...), in the order
of their names, with the time 1980-01-01 00:00 and the mode 0644 (0755 for an .exe), deflated at
level 9.  The zip is written to OUTPUT.zip.new and renamed when it is whole.
"""

import os
import sys
import zipfile


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: make-zip.py DIRECTORY OUTPUT.zip\n")
        return 2
    directory = os.path.normpath(sys.argv[1])
    output = sys.argv[2]
    top = os.path.basename(directory)

    # The files, by their names in the zip.
    names = []
    for root, folders, files in os.walk(directory):
        folders.sort()
        for name in files:
            path = os.path.join(root, name)
            inside = os.path.relpath(path, directory).replace(os.sep, "/")
            names.append((top + "/" + inside, path))
    names.sort()

    # Each file with the fixed time and its mode.
    with zipfile.ZipFile(output + ".new", "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as archive:
        for inside, path in names:
            info = zipfile.ZipInfo(inside, date_time=(1980, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o644
            if inside.endswith(".exe"):
                mode = 0o755
            info.external_attr = (0o100000 | mode) << 16
            info.create_system = 3
            with open(path, "rb") as source:
                archive.writestr(info, source.read(), compress_type=zipfile.ZIP_DEFLATED, compresslevel=9)
    os.replace(output + ".new", output)
    print("make-zip: %s: %d files" % (output, len(names)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
