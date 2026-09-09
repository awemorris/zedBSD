#!/usr/bin/env python3
"""Verify filename-safe completion records, skips and report failure boundaries."""
import os
from pathlib import Path
import stat
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-cpreport-") as temporary:
        work = Path(temporary)
        binary = work / "cp"
        sources = [REPO / "userland/base/cp/main.c", REPO / "userland/base/common/command.c"]
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*flags, *map(str, sources), "-o", str(binary)], check=True)
        source = work / "source"
        source.mkdir()
        (source / "dir").mkdir()
        (source / "empty").touch()
        (source / "a\nF\tspoof").write_bytes(b"contents")
        os.link(source / "empty", source / "dir/hardlink")
        (source / "link").symlink_to("dir")
        report = work / "report"
        count = 0

        def run(arguments, status=0, executable=binary):
            nonlocal count
            p = subprocess.run([str(executable), *map(str, arguments)], capture_output=True)
            assert p.returncode == status, (p.returncode, p.stderr)
            count += 1

        def records():
            lines = report.read_bytes().splitlines()
            assert lines[0] == b"CPCOPY1" and lines[-1] == b"END", lines
            result = []
            for line in lines[1:-1]:
                kind, path = line.split(b"\t")
                assert kind in (b"F", b"D")
                result.append((kind, bytes.fromhex(path.decode())))
            return result

        run(["-a", "-T", "--report-file=" + str(report), source, work / "target"])
        expected = {(b"D" if stat.S_ISDIR(p.lstat().st_mode) else b"F", os.fsencode(p)) for p in [source, *source.rglob("*")]}
        observed = records()
        assert set(observed) == expected and len(observed) == len(expected)
        assert observed[-1] == (b"D", os.fsencode(source))
        assert (work / "target/empty").stat().st_ino == (work / "target/dir/hardlink").stat().st_ino
        original_report = report.read_bytes()
        run(["--report-file=" + str(report), source / "empty", work / "other"], status=1)
        assert report.read_bytes() == original_report and not (work / "other").exists()
        run(["-a", "--report-file=" + str(source / "report"), source, work / "other"], status=1)
        assert not (source / "report").exists() and not (work / "other").exists()
        (work / "other").mkdir()
        run(["-a", "--report-file=" + str(work / "other/report"), source, work / "other"], status=1)
        assert list((work / "other").iterdir()) == []
        report.unlink()
        run(["-n", "--report-file=" + str(report), source / "empty", work / "target/empty"])
        assert records() == []
        report.unlink()
        skipped = work / "skipped"
        skipped.mkdir()
        (skipped / "empty").write_bytes(b"foreign")
        run(["-a", "-n", "--report-file=" + str(report), source / "empty", source / "dir/hardlink", skipped])
        assert records() == [(b"F", os.fsencode(source / "dir/hardlink"))]
        assert (skipped / "empty").read_bytes() == b"foreign"

        for fault in ("write", "close", "metadata"):
            wrapper = work / (fault + ".c")
            prefix = "#include <errno.h>\n#include <string.h>\n#include <sys/stat.h>\n#include <unistd.h>\n"
            if fault == "metadata":
                body = "int __wrap_futimens(int fd, const struct timespec t[2]) { (void)fd; (void)t; errno=EIO; return -1; }"
                wraps = ["-Wl,--wrap=futimens"]
            else:
                body = "static int report_fd=-1; ssize_t __real_write(int,const void *,size_t); int __real_close(int);\n"
                body += "ssize_t __wrap_write(int fd,const void *p,size_t n) { if(n==8 && !memcmp(p,\"CPCOPY1\\n\",8)) report_fd=fd;"
                if fault == "write":
                    body += "if(fd==report_fd && n>1 && !memcmp(p,\"F\\t\",2)) { errno=EIO; return -1; }"
                body += "return __real_write(fd,p,n); }\nint __wrap_close(int fd) { int r=__real_close(fd); if(r) return r;"
                if fault == "close":
                    body += "if(fd==report_fd) { errno=EIO; return -1; }"
                body += "return 0; }\n"
                wraps = ["-Wl,--wrap=write", "-Wl,--wrap=close"]
            wrapper.write_text(prefix + body)
            faulty = work / ("cp-" + fault)
            subprocess.run([*flags, *map(str, sources), str(wrapper), *wraps, "-o", str(faulty)], check=True)
            report.unlink()
            run(["-p", "--report-file=" + str(report), source / "empty", work / ("fault-" + fault)], status=1, executable=faulty)
            if fault != "close":
                assert report.read_bytes() == b"CPCOPY1\n"
            else:
                # END cannot override a failed child exit/close.
                assert records() == [(b"F", os.fsencode(source / "empty"))]
        # A last-file metadata failure leaves exactly the successful prefix.
        wrapper = work / "last-metadata.c"
        wrapper.write_text("#include <errno.h>\n#include <sys/stat.h>\n"
                           "static int calls; int __real_futimens(int,const struct timespec[2]);\n"
                           "int __wrap_futimens(int fd,const struct timespec t[2]) {"
                           "if(++calls==3) {errno=EIO; return -1;} return __real_futimens(fd,t);}\n")
        faulty = work / "cp-last"
        subprocess.run([*flags, *map(str, sources), str(wrapper), "-Wl,--wrap=futimens", "-o", str(faulty)], check=True)
        last_target = work / "last-target"
        last_target.mkdir()
        operands = [work / name for name in ("first", "second", "last")]
        for operand in operands:
            operand.touch()
        report.unlink()
        run(["-p", "--report-file=" + str(report), *operands, last_target], status=1, executable=faulty)
        lines = report.read_bytes().splitlines()
        assert lines == [b"CPCOPY1", *[b"F\t" + os.fsencode(p).hex().encode() for p in operands[:2]]], lines
        print("cp report PASS", count, "cases")


if __name__ == "__main__":
    main()
