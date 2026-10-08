#!/usr/bin/env python3
"""ws177-p024: checks that every object kind of the compositor reaches its requests' handler through protocol.c.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    host-dispatch-kinds.py [REPOSITORY]

T1-306 found a layer leak the host tests did not catch: kl_system_printers_v1's object kind had no case in
protocol.c's kwl_dispatch, so every printers request was refused, while host-system.c called system.c directly.  This
reads the sources: each kind of `enum kwl_kind` (wayland/kwl.h) must have a case in kwl_dispatch (protocol.c), and when
the case hands the request to a handler kwl_*_request that switches on object->kind, the handler's switch must have a
case for each kind handed to it.  Prints "PASS name" or "FAIL name ..." and exits 1 when one failed.
"""
import re
import sys
from pathlib import Path

ROOT = Path(sys.argv[1] if len(sys.argv) > 1 else Path(__file__).resolve().parents[3])
WAYLAND = ROOT / "userland/desktop/wayland"

# Kinds that take no request (a wl_callback is only sent done).
NO_REQUESTS = {"KWL_CALLBACK"}

failures = 0


def check(name, ok, detail=""):
	global failures
	print(("PASS " if ok else "FAIL ") + name + ("" if ok else ": " + detail))
	if not ok:
		failures += 1


def body_of(text, name):
	"""The body of a function defined as `name(` at a line's start (the definition style of the tree)."""
	match = re.search(r"^" + re.escape(name) + r"\(", text, re.M)
	if match is None:
		return None
	start = text.index("{", match.end())
	end = text.index("\n}\n", start)
	return text[start:end]


def strip_comments(text):
	return re.sub(r"/\*.*?\*/", "", text, flags=re.S)


def main():
	header = strip_comments((WAYLAND / "kwl.h").read_text())
	enum = re.search(r"enum kwl_kind \{(.*?)\};", header, re.S)
	kinds = [kind for kind in re.findall(r"\b(KWL_[A-Z0-9_]+)\b", enum.group(1))]
	check("kinds-read", len(kinds) > 20, str(len(kinds)))
	protocol = strip_comments((WAYLAND / "protocol.c").read_text())
	dispatch = body_of(protocol, "kwl_dispatch")
	check("dispatch-found", dispatch is not None)
	if dispatch is None:
		return 1
	cases = set(re.findall(r"case (KWL_[A-Z0-9_]+):", dispatch))
	missing = [kind for kind in kinds if kind not in cases and kind not in NO_REQUESTS]
	check("every-kind-dispatched", not missing, " ".join(missing))

	# The groups of cases handed to one handler, and each handler's own switch.
	sources = {path: strip_comments(path.read_text()) for path in WAYLAND.glob("*.c")}
	groups = re.findall(r"((?:\s*case KWL_[A-Z0-9_]+:)+)\s*error = (kwl_[a-z0-9_]+_request)\(", dispatch)
	for group, handler in groups:
		handed = re.findall(r"case (KWL_[A-Z0-9_]+):", group)
		body = None
		for text in sources.values():
			body = body_of(text, handler)
			if body is not None:
				break
		if body is None:
			check(f"{handler}-found", False, "no definition")
			continue
		switch = re.search(r"switch \(object->kind\) \{", body)
		if switch is None or len(handed) < 2:
			continue
		own = set(re.findall(r"case (KWL_[A-Z0-9_]+):", body[switch.end():]))
		lost = [kind for kind in handed if kind not in own]
		check(f"{handler}-takes-its-kinds", not lost, " ".join(lost))

	print("host-dispatch-kinds: " + ("PASS" if failures == 0 else f"FAIL {failures}"))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
