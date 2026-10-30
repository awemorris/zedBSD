#!/usr/bin/env python3
"""Loads damaged copies of AML tables in aml-host under the sanitizers.

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

    plan/tools/aml-host/fuzz.py [--iterations N] [--seed S] TABLE...

Each iteration takes one of the tables, changes a few bytes of its AML
(random values, or bytes typical of AML: opcodes, package lengths, name
characters), and loads it with aml-host --reg --init --methods, which also
runs every method without arguments.  Errors the interpreter reports are
fine; a sanitizer report, a crash or a hang is a failure, and the damaged
table is kept in build/aml-host/fuzz/ with its seed.
"""
import os
import random
import signal
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
HOST = os.path.join(REPO, "build", "aml-host", "aml-host")
OUT = os.path.join(REPO, "build", "aml-host", "fuzz")
TYPICAL = [0x00, 0x01, 0x0A, 0x0B, 0x0C, 0x0E, 0x10, 0x11, 0x12, 0x14, 0x5B, 0x5C, 0x5E,
	0x60, 0x68, 0x70, 0x72, 0x86, 0x88, 0x8A, 0x93, 0xA0, 0xA1, 0xA2, 0xA4, 0xFF, 0x3F, 0x7F, 0xBF]


def mutate(data, generator):
	data = bytearray(data)
	count = generator.randint(1, 8)
	for _ in range(count):
		position = generator.randrange(36, len(data))
		if generator.random() < 0.5:
			data[position] = generator.randrange(256)
		else:
			data[position] = generator.choice(TYPICAL)
	return bytes(data)


def main():
	arguments = sys.argv[1:]
	iterations = 200
	seed = 1
	while arguments and arguments[0].startswith("--"):
		if arguments[0] == "--iterations":
			iterations = int(arguments[1])
			arguments = arguments[2:]
		elif arguments[0] == "--seed":
			seed = int(arguments[1])
			arguments = arguments[2:]
		else:
			print(__doc__, file=sys.stderr)
			return 2
	if not arguments:
		print(__doc__, file=sys.stderr)
		return 2
	tables = []
	for path in arguments:
		with open(path, "rb") as stream:
			tables.append((path, stream.read()))
	os.makedirs(OUT, exist_ok=True)
	environment = dict(os.environ)
	environment["ASAN_OPTIONS"] = "exitcode=99:detect_leaks=1"
	environment["UBSAN_OPTIONS"] = "exitcode=98:halt_on_error=1:print_stacktrace=1"
	failures = 0
	codes = {}
	for iteration in range(iterations):
		generator = random.Random(seed * 1000003 + iteration)
		path, data = generator.choice(tables)
		damaged = mutate(data, generator)
		target = os.path.join(OUT, "case.aml")
		with open(target, "wb") as stream:
			stream.write(damaged)
		try:
			result = subprocess.run(
				[HOST, "--quiet", "--reg", "--init", "--methods", target],
				capture_output=True, text=True, timeout=60, env=environment)
			code = result.returncode
			report = result.stderr
		except subprocess.TimeoutExpired:
			code = "hang"
			report = ""
		codes[code] = codes.get(code, 0) + 1
		bad = code in (99, 98, "hang") or (isinstance(code, int) and code < 0)
		if bad:
			failures += 1
			kept = os.path.join(OUT, "failure-%d-%d.aml" % (seed, iteration))
			with open(kept, "wb") as stream:
				stream.write(damaged)
			with open(kept + ".log", "w") as stream:
				stream.write("source %s\ncode %s\n%s" % (path, code, report))
			print("FAILURE %s: iteration %d from %s (code %s)" % (kept, iteration, os.path.basename(path), code))
	print("fuzz: %d iterations, %d failures (seed %d); exit codes %s" % (iterations, failures, seed, sorted(codes.items(), key=str)))
	return 1 if failures else 0


if __name__ == "__main__":
	sys.exit(main())
