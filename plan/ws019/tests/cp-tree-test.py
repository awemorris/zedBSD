#!/usr/bin/env python3
"""Exercise physical recursive copying, metadata and directory failure paths."""
import os
from pathlib import Path
import socket
import stat
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-cptree-") as directory:
        work = Path(directory)
        binary = work / "cp"
        sources = [REPO / "userland/base/cp/main.c", REPO / "userland/base/common/command.c"]
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*flags, *map(str, sources), "-o", str(binary)], check=True)
        root = work / "source"
        root.mkdir()
        (root / "empty").mkdir()
        (root / "directory").mkdir()
        (root / "directory" / "file").write_bytes(bytes(range(251)) * 300)
        (root / "line\nname").touch()
        (root / "link").symlink_to("directory")
        (root / "dangling").symlink_to("absent")
        (root / "directory" / "cycle").symlink_to("..")
        os.mkfifo(root / "fifo", 0o640)
        count = 0

        def run(args, status=0, executable=binary):
            nonlocal count
            p = subprocess.run([str(executable), *map(str, args)], capture_output=True, timeout=10)
            assert p.returncode == status, (args, p.returncode, p.stderr)
            count += 1

        paths = [root, *root.iterdir(), root / "directory/file", root / "directory/cycle"]
        stamps = (1234567890123456789, 1234567900987654321)
        for path in paths:
            os.utime(path, ns=stamps, follow_symlinks=False)
        os.chmod(root / "empty", 0o550)
        os.chmod(root / "directory/file", 0o6750)
        before = {str(p.relative_to(root)): p.lstat() for p in paths}
        destination = work / "target"
        run(["-R", "-p", "-T", root, destination])
        for name, original in before.items():
            copied = (destination / name).lstat()
            assert copied.st_mode == original.st_mode, name
            assert (copied.st_uid, copied.st_gid) == (original.st_uid, original.st_gid), name
            assert (copied.st_atime_ns, copied.st_mtime_ns) == (original.st_atime_ns, original.st_mtime_ns), name
        assert (destination / "directory/file").read_bytes() == (root / "directory/file").read_bytes()
        assert os.readlink(destination / "link") == "directory"
        assert os.readlink(destination / "dangling") == "absent"
        assert os.readlink(destination / "directory/cycle") == ".."
        assert stat.S_ISFIFO((destination / "fifo").stat().st_mode)

        container = work / "container"
        container.mkdir()
        run(["-R", str(root) + "/", container])
        assert (container / "source/directory/file").is_file()
        assert not (container / "directory").exists()
        flat = work / "flat"
        flat.mkdir()
        run(["-R", str(root) + "/.", flat])
        assert (flat / "directory/file").is_file()
        run(["-R", "-T", root, root], status=1)
        run(["-R", "-T", root, root / "inside"], status=1)
        assert not (root / "inside").exists()
        (work / "alias").symlink_to(root)
        run(["-R", "-T", root, work / "alias/inside"], status=1)
        assert not (root / "inside").exists()

        protected = work / "protected"
        protected.mkdir()
        (protected / "marker").write_text("untouched")
        (flat / "directory/file").unlink()
        (flat / "directory/cycle").unlink()
        (flat / "directory").rmdir()
        (flat / "directory").symlink_to(protected)
        run(["-R", "-T", root / "directory", flat / "directory"], status=1)
        assert list(protected.iterdir()) == [protected / "marker"]
        run(["-R", "-n", "-T", root / "link", destination / "link"])
        run(["-R", "--update=none-fail", "-T", root / "link", destination / "link"], status=1)
        assert os.readlink(destination / "link") == "directory"
        # Archive preserves a group across directories and source operands.
        first = root / "hard-first"
        second = root / "directory/hard-second"
        first.write_bytes(b"shared inode content")
        os.link(first, second)
        archive = work / "archive"
        run(["-a", "-T", root, archive])
        copied_first = archive / "hard-first"
        copied_second = archive / "directory/hard-second"
        assert copied_first.stat().st_ino == copied_second.stat().st_ino
        assert copied_first.stat().st_ino != first.stat().st_ino
        copied_second.write_bytes(b"changed copy")
        assert copied_first.read_bytes() == b"changed copy"
        assert first.read_bytes() == b"shared inode content"
        operands = work / "operands"
        operands.mkdir()
        run(["-a", first, second, operands])
        assert (operands / "hard-first").stat().st_ino == (operands / "hard-second").stat().st_ino
        skipped = work / "skipped"
        skipped.mkdir()
        (skipped / "hard-first").write_bytes(b"foreign")
        run(["-a", "-n", first, second, skipped])
        assert (skipped / "hard-first").read_bytes() == b"foreign"
        assert (skipped / "hard-second").read_bytes() == b"shared inode content"
        assert (skipped / "hard-first").stat().st_ino != (skipped / "hard-second").stat().st_ino
        with socket.socket(socket.AF_UNIX) as endpoint:
            endpoint.bind(str(root / "socket"))
            run(["-R", "-T", root / "socket", work / "socket-copy"], status=1)
            assert not (work / "socket-copy").exists()

        for operation in ("readdir", "closedir", "lchown", "utimensat"):
            wrapper = work / (operation + ".c")
            if operation == "readdir":
                body = "struct dirent *__real_readdir(DIR *); struct dirent *__wrap_readdir(DIR *d) { static int n; if (++n == 4) { errno = EIO; return 0; } return __real_readdir(d); }"
            elif operation == "closedir":
                body = "int __real_closedir(DIR *); int __wrap_closedir(DIR *d) { int r = __real_closedir(d); if(r) return r; errno = EIO; return -1; }"
            elif operation == "lchown":
                body = "int __wrap_lchown(const char *p, uid_t u, gid_t g) { (void)p; (void)u; (void)g; errno = EPERM; return -1; }"
            else:
                body = "int __wrap_utimensat(int f, const char *p, const struct timespec t[2], int flags) { (void)f; (void)p; (void)t; (void)flags; errno = EIO; return -1; }"
            wrapper.write_text("#include <dirent.h>\n#include <errno.h>\n#include <sys/stat.h>\n#include <unistd.h>\n" + body + "\n")
            faulty = work / ("cp-" + operation)
            subprocess.run([*flags, *map(str, sources), str(wrapper), "-Wl,--wrap=" + operation, "-o", str(faulty)], check=True)
            # A link-only tree avoids an unrelated retained socket failure.
            sample = work / ("sample-" + operation)
            sample.mkdir()
            (sample / "link").symlink_to("missing")
            run(["-R", "-p", "-T", sample, work / ("failed-" + operation)], status=1, executable=faulty)
        print("cp tree PASS", count, "cases")


if __name__ == "__main__":
    main()
