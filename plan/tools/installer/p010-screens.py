#!/usr/bin/env python3
"""The installer's screens in QEMU, each photographed.

Boots IMAGE (make installer-image with plan/tools/installer/config-amd64-p010.mk and the harness's files) from USB with a
Venus GPU (the desktop's compositor needs Vulkan; plan/ws035/tests/zdesktop-guest.sh's set-up), a USB tablet, an empty
NVMe disk and a SATA disk of "another system" (GPT: an ESP with \\EFI\\BOOT\\BOOTX64.EFI, a Microsoft basic data
partition, free room).  The installer opens by itself; the script walks it as a person would, through QMP's keyboard
and tablet (every place clicked is a control the installer's log names: INSTALLER LAYOUT and CONTROL, in the session's
log read over SSH), and photographs each screen and dialog:

  S1, S2, S3 (partitions chosen), S4 and its notice, a partition created, S5, S6 and the no-swap dialog, S7 and the
  firmware's licences, the cancel dialog, S10 (with S4's writes), then S2 again, the NVMe disk whole, S7a, S8, the
  install dialog, S9, S11.

With --language ja the desktop's language is set to Japanese first (the setting ui.language that Settings' Languages
page writes, set to 1 as the live session's user with keiland-settings, userland/tests/keiland-settings, which is no
part of the image: IMAGE's build's bin/keiland-settings is copied in, so build it with
"make ... $(BUILD)/bin/keiland-settings" first), the installer's words follow without a restart
(INSTALLER LANGUAGE language=ja), and the same screens are photographed in Japanese.

The pictures go to OUT/shots/NN-name.png (QMP screendump of the Venus head, or its VNC when screendump has no surface).
The summary is a line a step and "p010-screens: PASS|FAIL OUT".

    python3 plan/tools/installer/p010-screens.py [--language ja] IMAGE

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import shlex
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from installer_walk import BASIC, ESP, ROOT, Failed, Run, blank, fat, gpt  # noqa: E402


def disks(run: Run):
	"""An empty NVMe disk, and a SATA disk of another system: its ESP with \\EFI\\BOOT\\BOOTX64.EFI, a basic data partition."""
	blank(run.out / "nvme.img", 12)
	other = run.out / "other.img"
	blank(other, 14)
	gpt(other, 512, (1, 300, ESP, "EFI system partition"), (301, 4096, BASIC, "Basic data partition"))
	fat(other, 1, 300, "OTHERESP", {"/EFI/BOOT/BOOTX64.EFI": b"MZ another system's loader\n"})
	return (f"-drive file={run.out}/nvme.img,if=none,id=p10n,format=raw,cache=unsafe -device nvme,serial=p010n,drive=p10n "
		f"-device ahci,id=p10ahci -drive file={other},if=none,id=p10s,format=raw,cache=unsafe -device ide-hd,drive=p10s,bus=p10ahci.0")


LIVE_USER = "user"
LANGUAGES = {"ja": 1}


def language(run: Run, code: str):
	"""Sets the desktop's language as the live session's user (ui.language) and waits for the installer to follow."""
	# The settings' client (userland/tests/keiland-settings) is no part of the image: it is the build's, copied in.
	client = Path(run.image).resolve().parent / "bin/keiland-settings"
	put = run.guest("put", str(client), "/tmp/keiland-settings", timeout=120)
	if put.returncode != 0:
		raise Failed(f"keiland-settings not copied from {client}: {put.stderr.strip()}")
	run.root("chmod 755 /tmp/keiland-settings", 60)
	mark = len(run.read_log())
	command = f"cd; export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0; /tmp/keiland-settings set ui.language {LANGUAGES[code]}"
	status, text = run.root(f"su {LIVE_USER} -c {shlex.quote(command)}", 60)
	if status != 0:
		raise Failed(f"keiland-settings: {status} {text.strip()}")
	run.wait_line(rf"INSTALLER LANGUAGE language={code}\b", 30, mark)
	time.sleep(1.5)
	run.wait_screen("s1-welcome")
	run.say(f"STEP language {code}")


def walk(run: Run, code: str):
	# S1: the installer opens by itself; the disks are read.
	run.wait_line(r"INSTALLER READY", 300)
	run.wait_line(r"INSTALLER PROBE read", 120)
	run.wait_screen("s1-welcome")
	if code:
		language(run, code)
	run.say("STEP s1 ok")
	run.shot("s1-welcome")

	# S2: the disks; the other system's SATA disk chosen.
	run.click("next")
	found = run.wait_screen("s2-disk")
	run.shot("s2-disk")
	sata = [n for n in found if n.startswith("disk-sd") and found[n][4] == 1]
	if not sata:
		raise Failed(f"no SATA disk to choose: {sorted(found)}")
	run.click(sata[0])
	run.shot("s2-disk-chosen")
	run.say(f"STEP s2 {sata[0]}")

	# S3: partitions chosen.
	run.click("next")
	run.wait_screen("s3-handling")
	run.click("chosen")
	run.shot("s3-handling")
	run.say("STEP s3 chosen")

	# S4: the notice, then a partition created.
	run.click("next")
	run.wait_screen("s4-edit")
	run.shot("s4-notice")
	run.key("ret")
	time.sleep(1.0)
	run.shot("s4-edit")
	mark = len(run.read_log())
	run.click("create")
	run.wait_line(r"INSTALLER EDIT (DONE|FAIL)", 180, mark)
	run.wait_line(r"INSTALLER PROBE read", 120, mark)
	time.sleep(1.0)
	run.shot("s4-created")
	_index, line = run.wait_line(r"INSTALLER EDIT (DONE|FAIL)", 5, mark)
	run.say(f"STEP s4 {line.split('INSTALLER ', 1)[1]}")

	# S5: the new partition.
	run.click("next")
	run.wait_screen("s5-root")
	run.click("root-", prefix=True)
	run.shot("s5-root")
	run.say("STEP s5 ok")

	# S6, and the dialog of no swap partition.
	run.click("next")
	run.wait_screen("s6-swap")
	run.shot("s6-swap")
	run.click("next")
	time.sleep(1.0)
	run.shot("s6-no-swap-dialog")
	run.key("ret")
	run.wait_screen("s7-items")
	run.say("STEP s6 ok")

	# S7, the firmware's licences.
	run.shot("s7-items")
	run.wheel(run.origin()[0] + 300, run.origin()[1] + 300, True, 10)
	time.sleep(1.0)
	run.shot("s7-items-lower")
	run.click("licenses")
	time.sleep(1.0)
	run.shot("s7-licenses")
	run.click("license-close")
	time.sleep(0.8)
	run.click("license-agree")
	run.say("STEP s7 ok")

	# Cancel: its dialog, then S10 with S4's writes.
	run.click("cancel")
	time.sleep(1.0)
	run.shot("cancel-dialog")
	run.key("ret")
	run.wait_screen("s10-failed")
	run.shot("s10-cancelled")
	run.say("STEP s10 ok")

	# Start over: the NVMe disk, whole.
	run.click("start-over")
	found = run.wait_screen("s2-disk")
	nvme = [n for n in found if n.startswith("disk-nvme") and found[n][4] == 1]
	if not nvme:
		raise Failed(f"no NVMe disk to choose: {sorted(found)}")
	run.click(nvme[0])
	run.click("next")
	run.wait_screen("s3-handling")
	run.click("whole")
	run.shot("s3-whole")
	run.click("next")
	run.wait_screen("s7-items")
	_screen, found = run.controls()
	if "license-agree" in found:
		pass
	run.say(f"STEP whole {nvme[0]}")

	# S7a: the user.
	run.click("next")
	run.wait_screen("s7a-user")
	run.click("user")
	run.type("alice")
	run.click("fullname")
	run.type("Alice")
	run.click("password")
	run.type("alice123")
	run.click("password-again")
	run.type("alice123")
	run.click("root-password")
	run.type("root123")
	run.click("root-password-again")
	run.type("root123")
	time.sleep(0.8)
	run.shot("s7a-user")
	run.say("STEP s7a ok")

	# S8, the erasure agreed, the install dialog.
	run.click("next")
	run.wait_screen("s8-confirm")
	run.click("erase-agree")
	run.shot("s8-confirm")
	run.click("next")
	time.sleep(1.0)
	run.shot("s8-install-dialog")
	mark = len(run.read_log())
	run.key("ret")
	run.wait_screen("s9-progress")
	run.wait_line(r"INSTALLER INSTALL step=6/", 600, mark)
	time.sleep(2.0)
	run.shot("s9-progress")
	run.say("STEP s9 ok")

	# S11.
	index, line = run.wait_line(r"INSTALLER SCREEN name=(s11-done|s10-failed)", 3600, mark)
	run.wait_screen(line.split("name=")[1])
	run.shot(line.split("name=")[1])
	if "s11-done" not in line:
		failed = [l for l in run.read_log()[mark:] if "INSTALLER INSTALL failed" in l or "FAIL" in l]
		raise Failed("the installation failed: " + " | ".join(failed[-3:]))
	run.say("STEP s11 ok")


def main():
	args = sys.argv[1:]
	code = ""
	if len(args) == 3 and args[0] == "--language" and args[1] in LANGUAGES:
		code = args[1]
		args = args[2:]
	if len(args) != 1:
		print(__doc__, file=sys.stderr)
		return 2
	run = Run(args[0], f"p010s-{code}." if code else "p010s.")
	status = 0
	try:
		run.start(disks(run))
		walk(run, code)
	except (Failed, subprocess.TimeoutExpired, OSError) as error:
		run.say(f"FAIL {error}")
		status = 1
	finally:
		try:
			lines = run.read_log()
			(run.out / "session.log").write_text("\n".join(l for l in lines if "INSTALLER" in l or "KWL" in l) + "\n")
		except Exception:  # noqa: BLE001
			pass
		run.stop()
	run.say(f"p010-screens: {'PASS' if status == 0 else 'FAIL'} {run.out.relative_to(ROOT)}")
	return status


if __name__ == "__main__":
	sys.exit(main())
