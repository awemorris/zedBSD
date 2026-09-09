#!/usr/bin/env python3
"""Independent tree comparison: differences, byte safety and incomplete reads."""
import os
from pathlib import Path
import shutil
import resource
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-diff-") as temporary:
        work = Path(temporary)
        sources = [REPO / ("userland/base/diff/" + name) for name in ("main.c", "tree.c")]
        sources.append(REPO / "userland/base/common/command.c")
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        binary = work / "diff"
        subprocess.run([*flags, *map(str, sources), "-o", str(binary)], check=True)
        a = work / "a"
        b = work / "b"
        a.mkdir()
        (a / "dir").mkdir()
        (a / "emptydir").mkdir()
        (a / "empty").touch()
        (a / os.fsdecode(b"raw-\xff")).touch()
        (a / "binary").write_bytes(b"\0first" + bytes(range(256)) * 100)
        (a / "line\nname").write_bytes(b"some text\n")
        os.link(a / "line\nname", a / "dir/hard")
        (a / "dir/cycle").symlink_to("..")
        (a / "dangling").symlink_to("missing")
        shutil.copytree(a, b, symlinks=True)
        (b / "dir/hard").unlink()
        os.link(b / "line\nname", b / "dir/hard")
        os.mkfifo(a / "fifo")
        os.mkfifo(b / "fifo")
        count = 0

        def reset_times():
            for tree in (a, b):
                for path in [*tree.rglob("*"), tree]:
                    os.utime(path, ns=(1700000000123456789, 1700000000987654321), follow_symlinks=False)

        def run(status=0, args=None, executable=binary):
            nonlocal count
            reset_times()
            result = subprocess.run([str(executable), *(args or ["-r", "-q", "--metadata", str(a), str(b)])], capture_output=True)
            assert result.returncode == status, (status, result.returncode, result.stdout, result.stderr)
            count += 1
            return result

        run()
        (b / "extra").touch()
        run(1)
        with open("/dev/full", "wb") as full:
            result = subprocess.run([str(binary), "-r", str(a), str(b)], stdout=full, stderr=subprocess.PIPE)
        assert result.returncode == 2, result
        count += 1
        (b / "extra").unlink()
        (b / "empty").unlink()
        run(1)
        (b / "empty").touch()
        (b / "binary").write_bytes(b"\0other" + bytes(range(256)) * 100)
        run(1)
        run(1, [str(a / "binary"), str(b / "binary")])
        shutil.copyfile(a / "binary", b / "binary")
        (b / "empty").chmod(0o600)
        run(1)
        run(0, ["-r", "-q", str(a), str(b)])
        (b / "empty").chmod((a / "empty").stat().st_mode & 0o7777)
        (b / "dangling").unlink()
        (b / "dangling").symlink_to("elsewhere")
        run(1)
        (b / "dangling").unlink()
        (b / "dangling").symlink_to("missing")
        (b / "dir/hard").unlink()
        shutil.copyfile(a / "dir/hard", b / "dir/hard")
        run(1)
        (b / "dir/hard").unlink()
        os.link(b / "line\nname", b / "dir/hard")
        (a / "independent").write_bytes(b"some text\n")
        os.link(b / "line\nname", b / "independent")
        run(1)
        (a / "independent").unlink()
        (b / "independent").unlink()
        run()
        run(2, ["-r", str(a), str(work / "missing")])
        run(1, ["-q", "/dev/null", "/dev/zero"])
        run(0, ["-q", "/dev/null", "/dev/null"])
        text_a, text_b = work / "text-a", work / "text-b"
        text_a.write_text("before\n")
        text_b.write_text("after\n")
        text = run(1, ["-u", str(text_a), str(text_b)]).stdout
        assert b"-before" in text and b"+after" in text
        reset_times()
        os.utime(b / "empty", ns=(1700000000123456789, 1700000000987654322))
        result = subprocess.run([str(binary), "-r", "--metadata", str(a), str(b)], capture_output=True)
        assert result.returncode == 1, result
        count += 1
        for fault in ("read", "early", "close", "readdir", "closedir", "short"):
            wrapper = work / (fault + ".c")
            body = "#include <unistd.h>\n#include <dirent.h>\n#include <errno.h>\n"
            symbol = fault
            if fault == "short":
                symbol = "read"
                body += "ssize_t __real_read(int,void *,size_t); ssize_t __wrap_read(int fd,void *p,size_t n) {return __real_read(fd,p,n>3?3:n);}\n"
            elif fault in ("read", "early"):
                symbol = "read"
                body += "ssize_t __wrap_read(int fd, void *p, size_t n) { (void)fd; (void)p; (void)n; errno=EIO; return " + ("0" if fault == "early" else "-1") + "; }\n"
            elif fault == "close":
                body += "int __real_close(int); int __wrap_close(int fd) { int r=__real_close(fd); if(fd>2) {errno=EIO; return -1;} return r;}\n"
            elif fault == "readdir":
                body += "struct dirent *__wrap_readdir(DIR *d) {(void)d; errno=EIO; return 0;}\n"
            else:
                body += "int __real_closedir(DIR *); int __wrap_closedir(DIR *d) {__real_closedir(d); errno=EIO; return -1;}\n"
            wrapper.write_text(body)
            injected = work / fault
            subprocess.run([*flags, *map(str, sources), str(wrapper), "-Wl,--wrap=" + symbol, "-o", str(injected)], check=True)
            run(0 if fault == "short" else 2, executable=injected)
        # The depth guard must return an error within the native 1 MiB stack budget.
        deep_a, deep_b = work / "deep-a", work / "deep-b"
        deep_a.mkdir()
        deep_b.mkdir()
        child_a, child_b = deep_a, deep_b
        for _ in range(130):
            child_a, child_b = child_a / "d", child_b / "d"
            child_a.mkdir()
            child_b.mkdir()
        def bound_stack():
            resource.setrlimit(resource.RLIMIT_STACK, (1048576, 1048576))
        result = subprocess.run([str(binary), "-r", "-q", str(deep_a), str(deep_b)],
                                capture_output=True, preexec_fn=bound_stack)
        assert result.returncode == 2, result
        count += 1
        print(f"diff tree PASS {count} cases")


if __name__ == "__main__":
    main()
