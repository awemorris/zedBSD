#!/usr/bin/env python3
# zedBSD; Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""The license inventory of a zedBSD image (ws129-p002).

For one or more image configurations (their union is the release), it asks make what the image holds -- the
userland packages selected after dependencies, the kernel options, the license files each package puts under
/usr/share/licenses/, the external packages' versions and the sources the kernel and the selected programs are built
from -- and checks it against tools/release/license-components.json:

  * every selected package is either the project's own (its sources say SPDX Zlib) or a listed component;
  * every non-Zlib SPDX line in the built sources lies under a component's paths;
  * every notice a present component needs is among the files the image installs (and, with --rootfs, on disk);
  * no component of the image is GPL-family unless it is marked for the user's decision ("decision", which fails
    the run until the user decides) or carries the user's decision ("decided" with its "decision" source);
  * with --distfiles, the external packages' archives are searched for GPL text (as audit-licenses.sh does).

It writes the inventory as Markdown (--markdown) and as the plain index the image could carry as
/usr/share/licenses/INDEX (--index): one line per component, "id<TAB>version<TAB>license<TAB>notice paths".
The exit status is 0 when nothing is missing, 1 when a gap, an unlisted package, an uncovered source or an
undecided GPL component remains, and 2 for a usage or make error.

  tools/release/license-inventory.py --config config/ci/config-amd64.mk \
      [--config plan/ws075/demo/config-demo-hdmi.mk] [--rootfs BUILD/rootfs] [--distfiles build/distfiles] \
      [--markdown OUT.md] [--index OUT.txt]
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tarfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
COMPONENTS = os.path.join(ROOT, "tools", "release", "license-components.json")
SPDX = re.compile(r"SPDX-License-Identifier:\s*([^*\n]+?)\s*(?:\*/)?\s*$", re.M)
GPL_TEXT = re.compile(rb"GNU General Public License|GNU Lesser General Public|SPDX-License-Identifier: *L?GPL")
GPL_LICENSE = re.compile(r"\bL?GPL\b|GNU General|GNU Lesser")
SOURCE_SUFFIXES = (".c", ".S", ".h", ".inc", ".cc", ".cpp")

# The make fragment that prints what an image holds, one record a line, fields split by "|".
DUMP = r"""
zedbsd-license-dump:
	@$(foreach p,$(ZEDBSD_USER_PROGRAMS),printf 'program|%s|%s\n' '$(p)' '$(USERLAND_$(p)_PACKAGE)';)
	@$(foreach v,$(filter CONFIG_DRIVER_%,$(.VARIABLES)),$(if $(filter y,$($(v))),printf 'driver|%s\n' '$(v)';))
	@$(foreach v,$(filter ZEDBSD_EXT_%_VERSION,$(.VARIABLES)),printf 'version|%s|%s\n' '$(patsubst ZEDBSD_EXT_%_VERSION,%,$(v))' '$($(v))';)
	@$(foreach v,$(filter ZEDBSD_EXT_%_ARCHIVE,$(.VARIABLES)),printf 'archive|%s|%s\n' '$(patsubst ZEDBSD_EXT_%_ARCHIVE,%,$(v))' '$($(v))';)
	@printf 'file|%s\n' $(ZEDBSD_PACKAGE_FILES) $(ZEDBSD_USERLAND_DATA_FILES)
	@$(foreach p,$(ZEDBSD_USER_PROGRAMS),$(foreach s,$(USERLAND_$(p)_SOURCES),printf 'source|%s|%s\n' '$(p)' '$(s)';))
	@printf 'kernel|%s\n' $(sort $(filter src/%,$(foreach v,$(filter %_SOURCES,$(.VARIABLES)),$($(v)))))
"""


def make_dump(config, scratch):
	"""Asks make what an image of a configuration holds; returns the records."""
	with open(os.path.join(scratch, "dump.mk"), "w") as fragment:
		fragment.write(DUMP)
	command = ["make", "-s", "-C", ROOT, "-f", "Makefile", "-f", os.path.join(scratch, "dump.mk"),
		   "ZEDBSD_CONFIG=" + config, "BUILD=" + os.path.join(scratch, "build"), "zedbsd-license-dump"]
	result = subprocess.run(command, capture_output=True, text=True, timeout=300)
	if result.returncode != 0:
		raise SystemExit("license-inventory: make failed for %s:\n%s" % (config, result.stderr[-2000:]))
	image = {"programs": {}, "drivers": set(), "versions": {}, "archives": {}, "licenses": {}, "sources": {}, "kernel": set()}
	words = []
	for line in result.stdout.splitlines():
		kind, _, rest = line.partition("|")
		fields = rest.split("|")
		if kind == "program":
			image["programs"][fields[0]] = fields[1]
		elif kind == "driver":
			image["drivers"].add(fields[0])
		elif kind == "version":
			image["versions"][fields[0]] = fields[1]
		elif kind == "archive":
			image["archives"][fields[0]] = fields[1]
		elif kind == "file":
			words.append(rest)
		elif kind == "source":
			image["sources"].setdefault(fields[0], set()).add(fields[1])
		elif kind == "kernel" and rest:
			image["kernel"].add(rest)
	# "--file DEST=SOURCE" pairs; "--mode" pairs are passed over.
	for index, word in enumerate(words):
		if word != "--file" or index + 1 >= len(words):
			continue
		destination, _, source = words[index + 1].partition("=")
		if destination.startswith("/usr/share/licenses/"):
			image["licenses"][destination] = source
	return image


def merge(images):
	"""The union of several images (the release is every configuration's packages)."""
	union = {"programs": {}, "drivers": set(), "versions": {}, "archives": {}, "licenses": {}, "sources": {}, "kernel": set()}
	for image in images:
		union["programs"].update(image["programs"])
		union["drivers"] |= image["drivers"]
		union["versions"].update(image["versions"])
		union["archives"].update(image["archives"])
		union["licenses"].update(image["licenses"])
		for program, sources in image["sources"].items():
			union["sources"].setdefault(program, set()).update(sources)
		union["kernel"] |= image["kernel"]
	return union


def present(component, image):
	"""Tells whether a component is in an image."""
	when = component["when"]
	if when == "always":
		return True
	kind, _, name = when.partition(":")
	if kind == "program":
		return name in image["programs"]
	if kind == "driver":
		return name in image["drivers"]
	return False


def spdx_lines(path):
	"""The SPDX license expressions of a source file (empty for none or an unreadable file)."""
	try:
		with open(os.path.join(ROOT, path), errors="replace") as source:
			text = source.read(65536)
	except OSError:
		return []
	return [match.strip().rstrip(".") for match in SPDX.findall(text)]


def built_files(image):
	"""The source files the image is built from, with the headers and includes beside them."""
	files = set(image["kernel"])
	for sources in image["sources"].values():
		files |= sources
	folders = set(os.path.dirname(path) for path in files)
	for folder in folders:
		directory = os.path.join(ROOT, folder)
		if not os.path.isdir(directory):
			continue
		for name in os.listdir(directory):
			if name.endswith(SOURCE_SUFFIXES):
				files.add(os.path.join(folder, name))
	return sorted(path for path in files if "/tests/" not in path and os.path.isfile(os.path.join(ROOT, path)))


def foreign_copyright(path):
	"""Tells whether a source file names a copyright holder other than the project's."""
	try:
		with open(os.path.join(ROOT, path), errors="replace") as source:
			text = source.read(65536)
	except OSError:
		return False
	for line in text.splitlines():
		if "Copyright" in line and "Awe Morris" not in line:
			return True
	return False


def scan_gpl(archive):
	"""The files of an archive whose text names a GPL; returns their names (tar archives only)."""
	found = []
	try:
		with tarfile.open(archive) as tar:
			for member in tar:
				if not member.isfile() or member.size > 4 * 1024 * 1024:
					continue
				data = tar.extractfile(member).read()
				if GPL_TEXT.search(data):
					found.append(member.name)
	except (tarfile.TarError, OSError):
		return None
	return found


def main():
	parser = argparse.ArgumentParser(description="The license inventory of a zedBSD image.")
	parser.add_argument("--config", action="append", required=True, help="an image configuration (repeat for a union)")
	parser.add_argument("--rootfs", help="a staged root filesystem whose /usr/share/licenses is checked")
	parser.add_argument("--distfiles", help="the external packages' archives, searched for GPL text")
	parser.add_argument("--components", default=COMPONENTS)
	parser.add_argument("--markdown")
	parser.add_argument("--index")
	parser.add_argument("--scratch", default=os.path.join(ROOT, "build", "license-inventory"))
	arguments = parser.parse_args()

	os.makedirs(arguments.scratch, exist_ok=True)
	with open(arguments.components) as table:
		components = json.load(table)["components"]
	image = merge([make_dump(config, arguments.scratch) for config in arguments.config])
	problems = []

	# The components the image holds, and the packages they cover.
	chosen = [component for component in components if present(component, image)]
	covered = set()
	for component in chosen:
		kind, _, name = component["when"].partition(":")
		if kind == "program":
			covered.add(name)

	# The project's own packages: their sources are Zlib (a package without sources is data or a program of the tree).
	own = sorted(program for program in image["programs"] if program not in covered)
	external_paths = ("packages/", "firmware/", "licenses/")
	for program in own:
		package = image["programs"][program]
		if package.startswith(external_paths) or "/" in package and package.split("/")[0] in ("libs", "network", "security", "fonts", "editors", "lang", "devel", "desktop"):
			if not image["sources"].get(program):
				problems.append("unlisted external package: %s (%s)" % (program, package))

	# Every non-Zlib SPDX line of the built sources under a present component's paths.
	licensed_paths = [(prefix, component["id"]) for component in chosen for prefix in component["paths"]]
	foreign = {}
	unlabeled = []
	for path in built_files(image):
		expressions = spdx_lines(path)
		owner = next((identifier for prefix, identifier in licensed_paths if path.startswith(prefix)), None)
		if not expressions:
			# A file under a component's paths is that component's; another with someone else's copyright is a gap.
			if owner is None and foreign_copyright(path):
				problems.append("uncovered third-party text without SPDX: %s" % path)
			elif owner is None:
				unlabeled.append(path)
			continue
		for expression in expressions:
			if expression == "Zlib":
				continue
			foreign.setdefault((owner, expression), []).append(path)
			if owner is None:
				problems.append("uncovered %s source: %s" % (expression, path))

	# The notices each present component needs, in the image (and on disk).
	rows = []
	for component in chosen:
		version = component["version"]
		if version.startswith("ext:"):
			version = image["versions"].get(version[4:], "?")
		missing = [notice for notice in component["notices"] if notice not in image["licenses"]]
		if arguments.rootfs:
			missing += [notice for notice in component["notices"]
				    if notice in image["licenses"] and not os.path.isfile(os.path.join(arguments.rootfs, notice.lstrip("/")))]
		status = component.get("status", "ok")
		if missing:
			status = "gap"
			for notice in missing:
				problems.append("missing notice of %s: %s" % (component["id"], notice))
		if GPL_LICENSE.search(component["license"]) and component.get("status") not in ("decision", "decided"):
			problems.append("GPL-family component without a decision: %s" % component["id"])
		if component.get("status") == "decision":
			problems.append("decision pending: %s (%s)" % (component["id"], component["license"]))
		if component.get("status") == "decided" and not component.get("decision"):
			problems.append("decided without the decision's source: %s" % component["id"])
		rows.append((component, version, status, missing))

	# Notices the image installs that no component names (kept, but listed).
	named = set(notice for component in chosen for notice in component["notices"])
	stray = sorted(notice for notice in image["licenses"] if notice not in named)

	# The external archives' GPL text, as audit-licenses.sh reports it.
	gpl_files = {}
	if arguments.distfiles:
		for component in chosen:
			if not component["version"].startswith("ext:"):
				continue
			archive = image["archives"].get(component["version"][4:])
			if not archive or not re.search(r"\.tar(\.[a-z0-9]+)?$|\.tgz$", archive):
				continue
			path = os.path.join(arguments.distfiles, archive)
			if not os.path.isfile(path):
				gpl_files[component["id"]] = None
				continue
			gpl_files[component["id"]] = scan_gpl(path)

	# The Markdown inventory.
	lines = ["# zedBSD image license inventory", "",
		 "Generated by `tools/release/license-inventory.py` from %s." % ", ".join("`%s`" % config for config in arguments.config),
		 "%d userland packages, %d kernel options, %d license files under /usr/share/licenses." %
		 (len(image["programs"]), len(image["drivers"]), len(image["licenses"])), "",
		 "| Component | Version | License | Notices in the image | Status |", "| --- | --- | --- | --- | --- |"]
	for component, version, status, missing in rows:
		notices = "<br>".join("`%s`%s" % (notice, " (missing)" if notice in missing else "") for notice in component["notices"]) or "—"
		lines.append("| %s | %s | %s | %s | %s |" % (component["name"], version or "—", component["license"], notices, status))
	lines += ["", "## Notes", ""]
	for component, version, status, missing in rows:
		if component["note"]:
			lines.append("- **%s**: %s" % (component["id"], component["note"]))
	lines += ["", "## Sources under other licenses than Zlib", "", "| Component | License | Files |", "| --- | --- | --- |"]
	for (owner, expression), paths in sorted(foreign.items(), key=lambda item: (str(item[0][0]), item[0][1])):
		lines.append("| %s | %s | %d (%s) |" % (owner or "**none**", expression, len(paths), ", ".join(sorted(set(os.path.dirname(p) for p in paths)))))
	lines += ["", "%d built source files of the project carry no SPDX line and no other copyright (listed in the run's output)." % len(unlabeled)]
	if stray:
		lines += ["", "License files no component names: " + ", ".join("`%s`" % notice for notice in stray)]
	if arguments.distfiles:
		lines += ["", "## GPL text in the external archives", "", "| Component | Files naming a GPL |", "| --- | --- |"]
		for identifier, found in sorted(gpl_files.items()):
			if found is None:
				lines.append("| %s | archive not found |" % identifier)
			else:
				lines.append("| %s | %d%s |" % (identifier, len(found), (": " + ", ".join(found[:6]) + (" ..." if len(found) > 6 else "")) if found else ""))
	lines += ["", "## Open items", ""]
	lines += ["- " + problem for problem in problems] or ["- none"]
	text = "\n".join(lines) + "\n"
	if arguments.markdown:
		with open(arguments.markdown, "w") as output:
			output.write(text)
	else:
		sys.stdout.write(text)

	# The plain index the image could carry.
	if arguments.index:
		with open(arguments.index, "w") as output:
			for component, version, status, missing in rows:
				output.write("%s\t%s\t%s\t%s\n" % (component["id"], version or "-", component["license"], " ".join(component["notices"]) or "-"))

	# The unlabeled files and the problems, for the log.
	for path in unlabeled:
		print("license-inventory: no SPDX line: %s" % path, file=sys.stderr)
	for problem in problems:
		print("license-inventory: %s" % problem, file=sys.stderr)
	print("license-inventory: %d components, %d open items" % (len(rows), len(problems)), file=sys.stderr)
	return 1 if problems else 0


if __name__ == "__main__":
	sys.exit(main())
