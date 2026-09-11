#!/usr/bin/env python3
"""Verify real preserved metadata and failures alongside existing staging modes."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-cp-") as directory:
        work = Path(directory)
        binary = work / "cp"
        sources = [REPO / "userland/base/cp/main.c", REPO / "userland/base/common/command.c"]
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*flags, *map(str, sources), "-o", str(binary)], check=True)
        source = work / "source"
        destination = work / "destination"
        payload = bytes(range(251)) * 1049
        source.write_bytes(payload)
        mode = 0o6751
        os.chmod(source, mode)
        stamps = (1234567890123456789, 1234567900987654321)
        count = 0

        def run(arguments, expected=0, executable=binary):
            nonlocal count
            result = subprocess.run([str(executable), *map(str, arguments)], capture_output=True)
            assert result.returncode == expected, (arguments, result.returncode, result.stderr)
            count += 1
            return result

        for existing in (False, True):
            for attributes_only in (False, True):
                destination.unlink(missing_ok=True)
                if existing:
                    destination.write_bytes(b"existing contents")
                    os.chmod(destination, 0o600)
                os.utime(source, ns=stamps)
                options = ["-p", "-T"]
                if attributes_only:
                    options.append("--attributes-only")
                run([*options, source, destination])
                before = source.stat()
                after = destination.stat()
                assert stat.S_IMODE(after.st_mode) == mode
                assert (after.st_uid, after.st_gid) == (before.st_uid, before.st_gid)
                assert (after.st_atime_ns, after.st_mtime_ns) == stamps
                expected = payload
                if attributes_only:
                    expected = b"existing contents" if existing else b""
                assert destination.read_bytes() == expected
        destination.unlink()
        run(["-T", "--attributes-only", "--preserve=mode", "--update=none-fail", source, destination])
        assert destination.read_bytes() == b"" and stat.S_IMODE(destination.stat().st_mode) == mode
        run(["-T", "--update=none-fail", source, destination], expected=1)
        assert destination.read_bytes() == b""
        run(["-n", source, destination])
        assert destination.read_bytes() == b""
        run(["-p", source, source], expected=1)
        assert source.read_bytes() == payload
        run([source, destination])
        assert destination.read_bytes() == payload

        # Link wrappers target actual failure boundaries; production has no knobs.
        wrappers = {
            "fchown": "int __wrap_fchown(int f, uid_t u, gid_t g) { (void)f; (void)u; (void)g; errno = EPERM; return -1; }",
            "fchmod": "int __wrap_fchmod(int f, mode_t m) { (void)f; (void)m; errno = EIO; return -1; }",
            "futimens": "int __wrap_futimens(int f, const struct timespec t[2]) { (void)f; (void)t; errno = EIO; return -1; }",
            "close": "int __real_close(int); int __wrap_close(int f) { int writable = (fcntl(f, F_GETFL) & O_ACCMODE) != O_RDONLY; int r = __real_close(f); if (r) return r; if (writable) { errno = EIO; return -1; } return 0; }"
        }
        for name, body in wrappers.items():
            wrapper = work / (name + ".c")
            wrapper.write_text("#include <errno.h>\n#include <fcntl.h>\n#include <sys/stat.h>\n#include <unistd.h>\n" + body + "\n")
            faulty = work / ("cp-" + name)
            subprocess.run([*flags, *map(str, sources), str(wrapper), "-Wl,--wrap=" + name, "-o", str(faulty)], check=True)
            destination.unlink(missing_ok=True)
            run(["-p", source, destination], expected=1, executable=faulty)
            assert source.read_bytes() == payload
        print("cp attributes PASS", count, "cases")


if __name__ == "__main__":
    main()
