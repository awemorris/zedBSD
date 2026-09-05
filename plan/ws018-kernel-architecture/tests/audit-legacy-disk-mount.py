#!/usr/bin/env python3
"""KA-T120: retired /diskN implementation must not regain active consumers."""
from pathlib import Path
import re
import subprocess

REPO = Path(__file__).resolve().parents[3]
SELF = Path(__file__).resolve()
retired = re.compile(
    r"\b(?:rootfs_type|rootfs_reset|rootfs_add_mountpoint|rootfs_remove_mountpoint|"
    r"mount_rootfs|mount_root_inode|mount_follow|mount_cross_parent|m_mountpoint|"
    r"INODE_MOUNTPOINT)\b|kern/rootfs\.[cho]\b")
paths = subprocess.check_output([
    "git", "ls-files", "--cached", "--others", "--exclude-standard", "-z", "--",
    "src", "include", "platform", "bootloader", "tools", "userland", "plan",
], cwd=REPO).decode().split("\0")
failures = []
count = 0
for name in sorted(set(paths)):
    path = REPO / name
    if not name or not path.is_file() or path.resolve() == SELF:
        continue
    if "temp" in path.parts or path.suffix not in {".c", ".h", ".S", ".mk", ".sh", ".py"}:
        continue
    count += 1
    for line, text in enumerate(path.read_text(errors="replace").splitlines(), 1):
        if retired.search(text):
            failures.append(f"{name}:{line}: retired dependency: {text.strip()}")
for name in ("src/kern/rootfs.c", "include/kern/rootfs.h"):
    if (REPO / name).exists():
        failures.append(f"retired file still exists: {name}")
for platform in ("amd64", "pcat", "pc98", "arm64", "sparcv9", "x68k"):
    path = REPO / f"platform/{platform}/vmunix.mk"
    if not path.is_file():
        failures.append(f"missing manifest: {path}")
flags = (REPO / "include/kern/inode.h").read_text()
for name, value in (("INODE_ROOT", 1), ("INODE_SWAPFILE", 0x10), ("INODE_LOOPFILE", 0x20)):
    match = re.search(rf"#define\s+{name}\s+(0x[0-9a-fA-F]+)U", flags)
    if not match or int(match[1], 16) != value:
        failures.append(f"retained inode flag value changed: {name}")
for name, symbols in {
    "src/kern/namei.c": ("mount_lookup_child", "mount_cross_path_parent"),
    "src/kern/file.c": ("mount_readdir_child",),
    "src/kern/vfs.c": ("mount_root_create", "VFS_LEGACY_NULL_AUTOROOT"),
}.items():
    text = (REPO / name).read_text()
    for symbol in symbols:
        if symbol not in text:
            failures.append(f"missing retained mechanism: {name}: {symbol}")
if failures:
    raise SystemExit("\n".join(failures))
print(f"KA-T120: PASS ({count} active source/test files; six manifests; retained flags/callers)")
