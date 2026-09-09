#!/usr/bin/env python3
"""Exercise packaged archive cp and report parsing on a disposable native UFS."""
import json
from pathlib import Path
import re
import runpy
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
I = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = I["F"]
REPO = I["REPO"]


def main():
    out = Path(sys.argv[1]).resolve()
    out.relative_to(REPO / "plan/ws019-installation/temp")
    out.mkdir(parents=True, exist_ok=False)
    result = F["prepare_boot"](out, False, False, REPO / "build/arch-images/amd64.ufs")
    F["create_nvme"](out / "gpt.img")
    before = F["digest"](out / "gpt.img")
    root = out / "native-root"
    root.mkdir()
    shutil.copyfile(HERE / "installer-treeprogress.noct", root / "treeprogress-test.noct")
    shutil.copyfile(HERE / "installer-treecopy.noct", root / "treecopy-test.noct")
    subprocess.run([str(REPO / "build/zedimage-host"), "ufs", "33554432",
                    str(root), str(out / "native.img")], check=True)
    guest = I["PublicInstallGuest"](out, usb_boot=True, extra_args=[
        "-drive", f"file={out / 'native.img'},format=raw,if=none,id=native",
        "-device", "usb-storage,drive=native,bus=xhci.0,port=2"])
    result["result"] = "FAIL native archive copy"
    try:
        guest.login()
        guest.run("mkdir /run/native")
        guest.run("mount -t ufs sdb /run/native")
        guest.run("/bin/noct --path=/lib/zedinst /run/native/treeprogress-test.noct",
                  "installer tree progress PASS 25 cases")
        guest.run("mkdir /run/native/source /run/native/source/nested")
        guest.run("echo native-copy > '/run/native/source/space name'")
        guest.run("touch /run/native/source/empty")
        guest.run("ln '/run/native/source/space name' /run/native/source/nested/hard")
        guest.run("ln -s '../space name' /run/native/source/nested/symbolic")
        guest.run("chown 17:23 '/run/native/source/space name'")
        guest.run("chmod 6751 '/run/native/source/space name'")
        source = guest.run("/bin/stat '/run/native/source/space name'")
        guest.run("/bin/find /run/native/source ! -type d -fprint0 /run/files")
        guest.run("/bin/cp -a --report-file=/run/report -- /run/native/source /run/native/copied")
        copied = guest.run("/bin/stat '/run/native/copied/space name'")
        assert "Uid: 17" in copied and "Gid: 23" in copied and "(6751)" in copied, copied
        # stat's public output proves seconds only; nanosecond acceptance is separate.
        for field in ("Access", "Modify"):
            expression = rf"^{field}: (\d+)$"
            assert re.search(expression, source, re.M).group(1) == re.search(expression, copied, re.M).group(1)
        hard = guest.run("/bin/stat /run/native/copied/nested/hard")
        inode = lambda text: re.search(r"Inode: (\d+)", text).group(1)
        assert inode(copied) == inode(hard) and inode(copied) != inode(source)
        symbolic = guest.run("/bin/stat /run/native/copied/nested/symbolic")
        assert "symbolic link" in symbolic, symbolic
        guest.run("cmp '/run/native/source/space name' '/run/native/copied/space name'")
        guest.run("cmp '/run/native/source/space name' /run/native/copied/nested/symbolic")
        report = guest.run("cat /run/report")
        entries = re.findall(r"^([FD])\t([0-9a-f]+)$", report, re.M)
        names = {(kind, bytes.fromhex(name).decode()) for kind, name in entries}
        expected = {("D", "/run/native/source"), ("D", "/run/native/source/nested")}
        expected.update(("F", "/run/native/source/" + name)
                        for name in ("space name", "empty", "nested/hard", "nested/symbolic"))
        assert len(entries) == len(expected) and names == expected, report
        assert re.search(r"^CPCOPY1$", report, re.M) and re.search(r"^END$", report, re.M), report
        guest.run("mkdir -m 700 /run/tree-workspace")
        guest.run("/bin/noct --path=/lib/zedinst /run/native/treecopy-test.noct /run/native/source /run/tree-workspace /run/native/monitored /bin/find /bin/cp ok",
                  "installer tree copy PASS ok")
        guest.run("cmp '/run/native/source/space name' /run/native/monitored/nested/hard")
        guest.run("/bin/cp -a --report-file=/run/report -- /run/native/source /run/native/other", status=1)
        guest.run("/bin/stat /run/native/other", status=1)
        guest.run("sync /run/native")
        guest.run("umount /run/native")
        guest.run("mount -t ufs sdb /run/native")
        persisted = guest.run("/bin/stat '/run/native/copied/space name'")
        assert inode(persisted) == inode(copied) and "(6751)" in persisted, persisted
        guest.run("cmp '/run/native/source/space name' /run/native/copied/nested/hard")
        guest.run("umount /run/native")
        result["guest_result"] = "PASS native UFS contents, owner/mode, second timestamps, hard/symbolic links, completion records and Noct parser"
        assert not re.search(r"panic:|fatal trap|assertion failed", guest.text(), re.I)
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        guest.stop()
        result["destination_before"] = before
        result["destination_after"] = F["digest"](out / "gpt.img")
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert before == result["destination_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    result["result"] = "PASS native archive copy"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
