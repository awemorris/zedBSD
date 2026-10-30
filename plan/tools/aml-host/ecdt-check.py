#!/usr/bin/env python3
"""Feeds aml-host broken ECDTs and checks that they are refused or survived.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    plan/tools/aml-host/ecdt-check.py [--iterations N] [--seed S]

Needs run-asl.py ecdt first (build/aml-host/asl/ecdt.aml and the support
ECDT).  Six hand-made broken tables must print the expected refusal, and
N (default 500) randomly changed tables must end with exit code 0 or 1
(never a sanitizer report, a signal or a hang).  Writes the tables to
build/aml-host/ecdt/.
"""
import argparse
import os
import random
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HOST = os.path.join(REPO, "build", "aml-host", "aml-host")
DSDT = os.path.join(REPO, "build", "aml-host", "asl", "ecdt.aml")
ECDT = os.path.join(REPO, "build", "aml-host", "asl", "support", "ecdt.aml")
OUT = os.path.join(REPO, "build", "aml-host", "ecdt")


def run(path):
	"""Runs the ECDT test's DSDT with an ECDT; returns (exit code, output)."""
	result = subprocess.run(
		[HOST, "--ecdt", path, "--reg", "--init", "--events", "--ec", "--ec-ram", "0x10=0x3c",
			"--ec-query", "0x42@0x16", "--main", DSDT],
		capture_output=True, text=True, timeout=60)
	return result.returncode, result.stdout + result.stderr


def main():
	parser = argparse.ArgumentParser()
	parser.add_argument("--iterations", type=int, default=500)
	parser.add_argument("--seed", type=int, default=7)
	arguments = parser.parse_args()
	os.makedirs(OUT, exist_ok=True)
	good = open(ECDT, "rb").read()
	failures = 0

	# The hand-made cases and the refusal each must print ("" accepts the table).
	cases = [
		("short", good[:60], "ecdt: error 22"),
		("unterminated", good[:-1], "ecdt: error 22"),
		("signature", b"ECDX" + good[4:], "ecdt: error 22"),
		("memory-space", good[:36] + b"\x00" + good[37:], "ecdt: error 95"),
		("zero-port", good[:52] + b"\x00" * 8 + good[60:], "ecdt: error 22"),
		("no-device", good[:65] + b"\\_SB.ECX\x00", ""),
	]
	for name, data, expected in cases:
		path = os.path.join(OUT, name + ".aml")
		with open(path, "wb") as stream:
			stream.write(data)
		code, output = run(path)
		refused = "ecdt: error" in output
		passed = code in (0, 1) and (expected in output if expected else not refused)
		print("%-13s %s (exit %d)" % (name, "passed" if passed else "FAILED", code))
		if not passed:
			failures += 1

	# The random changes: 1 to 6 bytes, and a cut end three times in ten.
	rng = random.Random(arguments.seed)
	codes = {}
	path = os.path.join(OUT, "fuzz.aml")
	for iteration in range(arguments.iterations):
		data = bytearray(good)
		for _ in range(rng.randint(1, 6)):
			data[rng.randrange(len(data))] = rng.randrange(256)
		cut = rng.randrange(len(data) + 1) if rng.random() < 0.3 else len(data)
		with open(path, "wb") as stream:
			stream.write(bytes(data[:cut]))
		code, output = run(path)
		codes[code] = codes.get(code, 0) + 1
		if code not in (0, 1):
			failures += 1
			kept = os.path.join(OUT, "failure-%d.aml" % iteration)
			os.replace(path, kept)
			print("fuzz %d: exit %d, kept %s" % (iteration, code, kept))
	print("ecdt: %d random tables, exit codes %s; %d failures" % (arguments.iterations, sorted(codes.items()), failures))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
