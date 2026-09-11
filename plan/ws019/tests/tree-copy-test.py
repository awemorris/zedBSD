#!/usr/bin/env python3
"""Run the real Noct census/monitor adapter with host-built zedBSD utilities."""
import os
from pathlib import Path
import subprocess
import tempfile
import time

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-treecopy-") as temporary:
        root = Path(temporary)
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        cp = root / "cp"
        find = root / "find"
        subprocess.run([*flags, str(REPO / "userland/base/cp/main.c"),
                        str(REPO / "userland/base/common/command.c"), "-o", str(cp)], check=True)
        subprocess.run([*flags, str(REPO / "userland/base/find/main.c"), "-o", str(find)], check=True)
        source = root / "source"
        source.mkdir()
        (source / "directory").mkdir()
        (source / "empty").touch()
        (source / "line\nname").write_bytes(b"native installer data")
        os.link(source / "line\nname", source / "directory/hard")
        (source / "link").symlink_to("directory/hard")
        count = 0
        for jit in ([], ["-j0"]):
            for fault in ("ok", "failed", "early", "status", "ui", "missing"):
                work = root / ("case" + str(count))
                work.mkdir(mode=0o700)
                workspace = work / "workspace"
                workspace.mkdir(mode=0o700)
                destination = work / "destination"
                command = cp
                if fault == "failed":
                    command = Path("/bin/false")
                elif fault in ("early", "status", "ui", "missing"):
                    command = work / "wrapper"
                    if fault == "early":
                        body = "import sys\nfrom pathlib import Path\np=next(a[14:] for a in sys.argv if a.startswith('--report-file='))\nPath(p).write_text('CPCOPY1\\nEND\\n')\n"
                    elif fault == "missing":
                        body = "pass\n"
                    else:
                        body = ("import os,sys,subprocess,time\nfrom pathlib import Path\n"
                                f"subprocess.run([{str(cp)!r}, *sys.argv[1:]], check=True)\n")
                        if fault == "status":
                            body += "sys.exit(1)\n"
                        else:
                            body += f"Path({str(work / 'pid')!r}).write_text(str(os.getpid()))\ntime.sleep(30)\n"
                    command.write_text("#!/usr/bin/python3\n" + body)
                    command.chmod(0o755)
                started = time.monotonic()
                result = subprocess.run([str(REPO / "build/NoctLang/build-static/noct"), *jit,
                    "--path=" + str(REPO / "userland/base/zedinst"),
                    str(REPO / "plan/ws019/tests/installer-treecopy.noct"),
                    str(source), str(workspace), str(destination), str(find), str(command), fault],
                    capture_output=True, text=True, timeout=45)
                assert result.returncode == 0, (fault, result.stdout, result.stderr)
                assert "installer tree copy PASS " + fault in result.stdout, result.stdout
                assert "Files copied: 0 / 4" in result.stdout, result.stdout
                if fault == "ok":
                    assert (destination / "line\nname").read_bytes() == b"native installer data"
                    assert (destination / "line\nname").stat().st_ino == (destination / "directory/hard").stat().st_ino
                if fault == "ui":
                    assert time.monotonic() - started < 15, "child was not promptly reaped"
                    pidfile = work / "pid"
                    if pidfile.exists():
                        try:
                            os.kill(int(pidfile.read_text()), 0)
                        except ProcessLookupError:
                            pass
                        else:
                            raise AssertionError("copy wrapper survived UI failure")
                count += 1
        print(f"tree copy adapter PASS {count} cases")


if __name__ == "__main__":
    main()
