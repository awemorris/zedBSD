#!/usr/bin/env python3
"""Check complete, filename-safe find output and injected traversal failures."""
import os
from pathlib import Path
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]


def main():
    with tempfile.TemporaryDirectory(prefix="zedbsd-find-") as directory:
        work = Path(directory)
        binary = work / "find"
        source = REPO / "userland/base/find/main.c"
        flags = ["cc", "-D_DEFAULT_SOURCE", "-I" + str(REPO), "-Wall", "-Wextra", "-Werror"]
        subprocess.run([*flags, str(source), "-o", str(binary)], check=True)
        root = work / "tree"
        root.mkdir()
        names = ["ordinary", ".hidden", "space name", "tab\tname", "line\nname", "-print", "quote'\"name"]
        for name in names:
            (root / name).touch()
        (root / "directory").mkdir()
        (root / "directory" / "nested").touch()
        (root / "link").symlink_to("directory")
        expected = {os.fsencode(p) for p in [root, *root.iterdir(), root / "directory" / "nested"]}
        count = 0

        def run(args, status=0, executable=binary, **options):
            nonlocal count
            result = subprocess.run([str(executable), *map(str, args)], capture_output=True, **options)
            assert result.returncode == status, (args, result.returncode, result.stderr)
            count += 1
            return result.stdout

        data = run([root, "-print0"])
        assert data.endswith(b"\0")
        records = data[:-1].split(b"\0")
        assert set(records) == expected and len(records) == len(expected)
        manifest = work / "manifest"
        assert run([root, "-fprint0", manifest]) == b""
        assert manifest.read_bytes() == data
        assert run([root, "-name", "-print"]) == os.fsencode(root / "-print") + b"\n"
        assert run([root, "-exec", "/bin/true", ";"]) == b""
        assert run([root, "-prune", "-print0"]) == os.fsencode(root) + b"\0"
        assert run([root, "-name", "impossible", "-a", "-print0"]) == b""
        assert run([root, "-name", "ordinary", "-o", "-name", ".hidden"]).splitlines() == [os.fsencode(p) for p in root.iterdir() if p.name in ("ordinary", ".hidden")]

        plain = work / "plain"
        plain.mkdir()
        (plain / "one").touch()
        lines = run([plain]).splitlines()
        assert lines == [os.fsencode(plain), os.fsencode(plain / "one")]
        assert run([], cwd=plain).splitlines() == [b".", b"./one"]
        assert run([plain, "-type", "f"]).splitlines() == [os.fsencode(plain / "one")]
        run([root, "-fprint0", "/dev/full"], status=1)
        with open("/dev/full", "wb") as full:
            result = subprocess.run([str(binary), str(root), "-print0"], stdout=full, stderr=subprocess.PIPE)
            assert result.returncode == 1
            count += 1
        run([work / "absent", "-print0"], status=1)
        run([root, "-fprint0", work / "absent/output"], status=2)
        run([root, "-fprint0", manifest, "-invalid"], status=2)
        (root / "directory" / "cycle").symlink_to("..")
        run(["-L", root, "-print0"], status=1)
        depth = work / "deep"
        depth.mkdir()
        for _ in range(130):
            depth = depth / "d"
            depth.mkdir()
        run([work / "deep", "-print0"], status=1)

        # Link-time wrappers inject faults without production environment knobs.
        for operation in ("readdir", "closedir"):
            wrapper = work / (operation + ".c")
            if operation == "readdir":
                body = "struct dirent *__real_readdir(DIR *);\nstruct dirent *__wrap_readdir(DIR *s) { static int calls; if (++calls == 4) { errno = EIO; return 0; } return __real_readdir(s); }\n"
            else:
                body = "int __real_closedir(DIR *);\nint __wrap_closedir(DIR *s) { int r = __real_closedir(s); if (r) return r; errno = EIO; return -1; }\n"
            wrapper.write_text("#include <dirent.h>\n#include <errno.h>\n" + body)
            faulty = work / ("find-" + operation)
            subprocess.run([*flags, str(source), str(wrapper), "-Wl,--wrap=" + operation, "-o", str(faulty)], check=True)
            run([root, "-print0"], status=1, executable=faulty)
        print("find manifest PASS", count, "cases")


if __name__ == "__main__":
    main()
