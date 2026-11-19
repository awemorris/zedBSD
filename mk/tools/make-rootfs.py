#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""Makes the UFS image of a root file system.

  make-rootfs.py --backend ZEDIMAGE-HOST --profile i386|amd64|aarch64|riscv64
                 --output IMAGE [--size-mib N] [--min-inodes N] [--force]
                 (--tree DIR | --file DEST=SOURCE... [--subtree DEST=DIR...]
                  [--mode DEST=MODE...])

With --tree the image carries DIR, a root the build already staged.
Otherwise a root is staged from the listed files and directories.  The
image itself is written by zedimage-host.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from zedbuild import fail, parse_number  # noqa: E402
from zedimage import count_entries, measure_mib, ufs_build, ufs_build_mib  # noqa: E402

PROFILES = ("i386", "amd64", "aarch64", "riscv64")
STAGED_DIRECTORIES = ("bin", "sbin", "lib", "etc", "var", "root", "home",
                      "usr/bin", "dev", "boot", "tmp", "run", "shm",
                      "var/empty", "usr/sbin", "usr/libexec", "etc/ssh")


def split_spec(spec: str, option: str) -> tuple[str, str]:
    at = spec.find("=")
    if at <= 0:
        fail(option + " requires DESTINATION=VALUE")
    return spec[:at], spec[at + 1:]


def run(command: list[str]) -> bool:
    return subprocess.run(command).returncode == 0


def remove_tree(path: str) -> bool:
    try:
        if os.path.lexists(path):
            shutil.rmtree(path)
    except OSError:
        return False
    return True


def publish(output: str) -> None:
    try:
        os.replace(output + ".tmp", output)
    except OSError:
        fail("cannot publish architecture image")


def wanted_inodes(root: str, min_inodes: int) -> int:
    """Half again as many inodes as entries, so that a later package does
    not have to be matched by a change here."""
    want = count_entries(root) * 3 // 2 + 256
    return max(want, min_inodes)


def from_tree(backend: str, tree: str, output: str, size_mib: int,
              min_inodes: int) -> None:
    """Makes the image of a finished root that the build already staged.
    Staging it a second time from a file list would copy through a second
    set of rules, and symbolic links, directory modes and anything added
    to the tree by a package would not survive it."""
    if not os.path.isdir(tree):
        fail("staged root is not a directory: " + tree)
    if not os.path.isfile(os.path.join(tree, "bin/sh")):
        fail("staged root has no /bin/sh: " + tree)
    if size_mib == 0:
        size_mib = measure_mib(tree)
    ufs_build_mib(backend, size_mib, tree, output + ".tmp", "ordinary",
                  wanted_inodes(tree, min_inodes))
    publish(output)


def stage(root: str, profile: str, files: list[str], subtrees: list[str],
          modes: list[str]) -> None:
    """Stages a root from the listed files and directories.

    Everything is written with the file creation mask 022, whatever the
    builder's is: the image's inodes take the staged modes and belong to
    root:wheel, so a mask of 002 would let the wheel group rewrite the
    system's files.  Group and other write bits come only from --mode, and
    the files are copied without -p so that the mask applies to them.
    """
    os.umask(0o022)
    if not remove_tree(root):
        fail("cannot prepare architecture staging root")
    try:
        # A directory is made with the mode it should have rather than the
        # one the builder's file creation mask happens to leave.  A
        # group-writable directory is refused by anything that moves its
        # root into the tree, because whoever is in that group could have
        # put something there.
        for name in STAGED_DIRECTORIES:
            os.makedirs(os.path.join(root, name), exist_ok=True)
        os.chmod(root, 0o755)
        for name in STAGED_DIRECTORIES:
            os.chmod(os.path.join(root, name), 0o755)
        os.symlink("../run", os.path.join(root, "var/run"))
        os.chmod(os.path.join(root, "tmp"), 0o1777)
        os.chmod(os.path.join(root, "shm"), 0o1777)
    except OSError:
        fail("cannot prepare architecture staging root")
    with open(os.path.join(root, "lib/arch.id"), "w") as stream:
        stream.write(profile + "\n")
    with open(os.path.join(root, "etc/zedbsd-root"), "w") as stream:
        stream.write("zedBSD ufs root v1\n")
    try:
        os.chmod(os.path.join(root, "lib/arch.id"), 0o644)
        os.chmod(os.path.join(root, "etc/zedbsd-root"), 0o644)
    except OSError:
        fail("cannot set the root marker's mode")

    have_sh = False
    for spec in files:
        dest, source = split_spec(spec, "--file")
        if dest == "/bin/sh":
            have_sh = True
        target = root + dest
        if not (run(["mkdir", "-p", os.path.dirname(target)]) and
                run(["cp", source, target])):
            fail("cannot install manifest file: " + dest)
    if not have_sh:
        fail("profile manifest must contain /bin/sh")
    # Each directory's files keep their modes less the mask, and its
    # symbolic links stay links (cp -RP).
    for spec in subtrees:
        dest, source = split_spec(spec, "--subtree")
        target = root + dest
        if not (os.path.isdir(source) and run(["mkdir", "-p", target]) and
                run(["cp", "-RP", source + "/.", target + "/"])):
            fail("cannot install manifest directory: " + dest)
    for spec in modes:
        dest, mode = split_spec(spec, "--mode")
        if not run(["chmod", mode, root + dest]):
            fail("cannot apply mode: " + spec)
    # The directories made for the files (/lib/firmware and the like) take
    # 0755 as the prepared ones do, not the builder's file creation mask;
    # the sticky ones keep theirs.
    if not run(["find", root, "(", "-path", root + "/tmp", "-o", "-path",
                root + "/shm", ")", "-prune", "-o", "-type", "d", "-exec",
                "chmod", "0755", "{}", "+"]):
        fail("cannot set the directories' mode")


def from_list(backend: str, profile: str, output: str, size_mib: int,
              min_inodes: int, files: list[str], subtrees: list[str],
              modes: list[str]) -> None:
    root = output + ".root.tmp"
    stage(root, profile, files, subtrees, modes)
    if size_mib == 0:
        size_mib = measure_mib(root)
    # A package's directory can be thousands of small files, more than the
    # inodes the size alone provisions: they are counted then, as for
    # --tree.  A root of listed files only is made as it always was.
    if subtrees:
        ufs_build_mib(backend, size_mib, root, output + ".tmp", "ordinary",
                      wanted_inodes(root, min_inodes))
    else:
        ufs_build(backend, str(size_mib * 1024 * 1024), root, output + ".tmp")
    publish(output)
    if not remove_tree(root):
        fail("cannot publish architecture image")


def main(args: list[str]) -> None:
    profile = ""
    output = ""
    backend = ""
    tree = ""
    size_mib = 0
    min_inodes = 0
    files: list[str] = []
    subtrees: list[str] = []
    modes: list[str] = []
    i = 0
    while i < len(args):
        has_value = i + 1 < len(args)
        if args[i] == "--profile" and has_value:
            profile = args[i + 1]
        elif args[i] == "--output" and has_value:
            output = args[i + 1]
        elif args[i] == "--backend" and has_value:
            backend = args[i + 1]
        elif args[i] == "--tree" and has_value:
            tree = args[i + 1]
        elif args[i] == "--size-mib" and has_value:
            size_mib = parse_number(args[i + 1])
        # A root that is written to later (the native root partition) asks
        # for inodes beyond what the staged tree needs.
        elif args[i] == "--min-inodes" and has_value:
            min_inodes = parse_number(args[i + 1])
        elif args[i] == "--file" and has_value:
            files.append(args[i + 1])
        elif args[i] == "--mode" and has_value:
            modes.append(args[i + 1])
        # A package's whole directory (DESTINATION=DIRECTORY), as the
        # root's tree rule takes it.
        elif args[i] == "--subtree" and has_value:
            subtrees.append(args[i + 1])
        elif args[i] == "--force":
            # Accepted for the callers; the image is always replaced.
            i += 1
            continue
        else:
            fail("invalid make-rootfs argument: " + args[i])
        i += 2
    if (profile not in PROFILES or output == "" or backend == "" or
            (size_mib != 0 and size_mib < 16)):
        fail("missing/invalid arch UFS arguments")

    if tree != "":
        if files or modes or subtrees:
            fail("--tree takes no --file, --subtree or --mode")
        from_tree(backend, tree, output, size_mib, min_inodes)
        return
    from_list(backend, profile, output, size_mib, min_inodes, files,
              subtrees, modes)


if __name__ == "__main__":
    main(sys.argv[1:])
