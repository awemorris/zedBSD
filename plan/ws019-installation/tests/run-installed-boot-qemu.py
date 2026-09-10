#!/usr/bin/env python3
"""Boot an accepted installation with no source disk; verify normal persistence."""
import json
from pathlib import Path
import re
import runpy
import shutil
import socket
import subprocess
import sys
import time

HERE = Path(__file__).resolve().parent
PUBLIC = runpy.run_path(str(HERE / "run-install-qemu.py"))
F = PUBLIC["F"]
REPO = PUBLIC["REPO"]


class InstalledGuest(PUBLIC["PublicInstallGuest"]):
    def __init__(self, output, disk, variables, extra_args=None, memory_mib=512, auxiliary_first=True):
        output.mkdir()
        self.output = output
        self.log = output / "guest.log"
        self.deadline = time.monotonic() + 600
        self.commands = (output / "commands.log").open("w")
        self.monitor = (output / "qemu.log").open("w")
        self.qmp = output / "qmp.sock"
        assert isinstance(memory_mib, int) and 64 <= memory_mib <= 4096
        args = ["qemu-system-x86_64", "-machine", "q35", "-m", str(memory_mib), "-smp", "4",
                "-drive", "if=pflash,format=raw,readonly=on,file=/usr/share/OVMF/OVMF_CODE_4M.fd",
                "-drive", f"if=pflash,format=raw,file={variables}",
                "-drive", f"file={disk},format=raw,if=none,id=installed",
                "-device", "nvme,drive=installed,serial=ws019installed,bootindex=1",
                "-nic", "none", "-display", "none", "-serial", "none",
                "-debugcon", f"file:{self.log}", "-monitor", "stdio",
                "-qmp", f"unix:{self.qmp},server=on,wait=off"]
        # Place an auxiliary controller before the installed one to exercise
        # discovery independently of enumeration order, without boot priority.
        if auxiliary_first:
            args[7:7] = extra_args or []
        else:
            args.extend(extra_args or [])
        self.commands.write(repr(args) + "\n")
        self.commands.flush()
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=self.monitor,
                                     stderr=subprocess.STDOUT, text=True)

    def check_root(self):
        text = self.text()
        assert "boot0=PARTUUID=78190000-2222-4222-8222-222222222222" in text, text
        assert "vfs: root=overlay lower=boot0:rootfs.img upper=boot0:data.img" in text, text
        assert "swap: swap0 source=boot0:swapfile slots=16383" in text, text
        assert "swap: active sources=1 total=16383" in text, text
        self.run("mount", r" on / type overlay ")

    def halt_checked(self):
        samples = []
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(10)
            connection.connect(str(self.qmp))
            with connection.makefile("rwb", buffering=0) as channel:
                json.loads(channel.readline())
                serial = 0

                def command(name, arguments=None):
                    nonlocal serial
                    serial += 1
                    request = {"execute": name, "id": serial}
                    if arguments is not None:
                        request["arguments"] = arguments
                    channel.write((json.dumps(request) + "\n").encode())
                    while True:
                        reply = json.loads(channel.readline())
                        if reply.get("id") == serial:
                            assert "error" not in reply, reply
                            return reply["return"]

                command("qmp_capabilities")
                self.send("halt")
                end = time.monotonic() + 75
                while time.monotonic() < end:
                    registers = command("human-monitor-command", {"command-line": "info registers -a"})
                    states = re.findall(r"RIP=([0-9a-f]+).*?RFL=([0-9a-f]+).*?HLT=([01])", registers, re.S)
                    terminal = len(states) == 4 and all(int(h) == 1 and int(flags, 16) & 0x200 == 0 for _, flags, h in states)
                    samples.append({"terminal": terminal, "registers": registers})
                    if len(samples) >= 3 and all(s["terminal"] for s in samples[-3:]):
                        break
                    time.sleep(1)
        (self.output / "halt-samples.json").write_text(json.dumps(samples, indent=2) + "\n")
        assert len(samples) >= 3 and all(s["terminal"] for s in samples[-3:]), "normal halt not established"
        assert not re.search(r"panic:|fatal trap|assertion failed|driver shutdown failed|controller stop failed", self.text(), re.I)


def main():
    accepted = Path(sys.argv[1]).resolve()
    conflict = Path(sys.argv[2]).resolve()
    out = Path(sys.argv[3]).resolve()
    for path in (accepted, conflict, out):
        path.relative_to(REPO / "plan/ws019-installation/temp")
    prior = json.loads((accepted / "result.json").read_text())
    verified = json.loads((conflict / "result.json").read_text())
    assert all(c in prior["cases"] for c in ("cancel", "install", "rerun"))
    assert verified["result"] == "PASS installer conflict continuation"
    assert verified["accepted_run"] == str(accepted)
    source = accepted / "gpt.img"
    source_hash = F["digest"](source)
    assert source_hash == verified["accepted_target_after"]
    assert prior["production_sha256"] == F["digest"](REPO / "build/amd64/hdd-image.img")
    out.mkdir(parents=True, exist_ok=False)
    F["copy_image"](source, out / "installed.img")
    shutil.copyfile(accepted / "vars.fd", out / "vars.fd")
    config = subprocess.check_output(["mtype", "-i", f"{source}@@{526336 * 512}", "::/zedbsd.cfg"]).decode()
    result = {"result": "FAIL installed boot/persistence", "source_sha256": source_hash,
              "production_sha256": prior["production_sha256"], "config": config, "cases": []}
    try:
        for index in range(2):
            guest = InstalledGuest(out / f"boot{index}", out / "installed.img", out / "vars.fd")
            try:
                guest.login()
                guest.check_root()
                if index == 0:
                    guest.run("echo ws019-q160-persistence > /root/ws019-persistence")
                    guest.run("sync /root/ws019-persistence")
                guest.run("cat /root/ws019-persistence", r"^ws019-q160-persistence$")
                if index == 1:
                    at = len(guest.text())
                    guest.send("reboot")
                    guest.login(at)
                    guest.check_root()
                    guest.run("cat /root/ws019-persistence", r"^ws019-q160-persistence$")
                guest.halt_checked()
                result["cases"].append("initial boot/write/halt" if index == 0 else "cold boot/reboot/persistence/halt")
            finally:
                guest.stop()
    except BaseException as error:
        result["failure"] = str(error)
        raise
    finally:
        result["source_sha256_after"] = F["digest"](source)
        result["production_sha256_after"] = F["digest"](REPO / "build/amd64/hdd-image.img")
        (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    assert source_hash == result["source_sha256_after"]
    assert result["production_sha256"] == result["production_sha256_after"]
    result["result"] = "PASS installed boot/persistence; selection fault cells remain"
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(result["result"])


if __name__ == "__main__":
    main()
