#!/usr/bin/env python3
"""Generates the tables and constants of zedBSD's libm.

 python3 src/libc/math/gen/gen-tables.py

writes src/libc/math/tables.c and src/libc/math/math-constants.h.  The
output is committed; running this again must reproduce it exactly.

Every value is computed with mpmath at 256 bits and rounded to the
nearest double; a double-double is that double and the nearest double
to the rest.  Nothing here is taken from another library: the tables
are values of elementary functions at chosen points, the constants are
splittings of ln 2, pi and friends into pieces with a stated number of
bits (the method of Cody and Waite), and the polynomial coefficients
are Taylor coefficients (exact rationals rounded to the nearest
double) unless a comment says otherwise.

zedBSD
Copyright (C) 2026 Awe Morris
SPDX-License-Identifier: Zlib
"""
from __future__ import annotations

import os
import re
import struct
import subprocess
import tempfile
from fractions import Fraction

import mpmath
from mpmath import mp, mpf

mp.prec = 256

HERE = os.path.dirname(os.path.abspath(__file__))
OUT_TABLES = os.path.join(HERE, "..", "tables.c")
OUT_CONSTANTS = os.path.join(HERE, "..", "math-constants.h")

HEADER = """/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

"""


def bits_double(bits: int) -> float:
	"""The double of an encoding."""
	return struct.unpack("<d", struct.pack("<Q", bits))[0]


def double_bits(value: float) -> int:
	"""The encoding of a double."""
	return struct.unpack("<Q", struct.pack("<d", value))[0]


def nearest(value) -> float:
	"""The double nearest to a high-precision value."""
	return float(mpf(value))


def dd(value) -> tuple[float, float]:
	"""A double-double: the nearest double and the nearest double to the rest."""
	high = nearest(value)
	low = nearest(mpf(value) - mpf(high))
	return high, low


def truncate_bits(value, bits: int) -> float:
	"""The value rounded to nearest with only `bits` significant bits."""
	value = mpf(value)
	exponent = int(mpmath.floor(mpmath.log(abs(value), 2)))
	scale = mpf(2) ** (bits - 1 - exponent)
	result = mpmath.nint(value * scale) / scale
	return nearest(result)


def literal(value: float) -> str:
	"""A C literal that converts back to exactly this double."""
	if value == 0.0:
		if double_bits(value) >> 63:
			return "-0.0"
		return "0.0"
	text = repr(value)
	if "e" not in text and "." not in text:
		text += ".0"
	return text


def significant_bits(value: float) -> int:
	"""How many significant bits a double has."""
	if value == 0.0:
		return 0
	fraction = Fraction(value)
	numerator = abs(fraction.numerator)
	while numerator % 2 == 0:
		numerator //= 2
	return numerator.bit_length()


class Output:
	"""Collects the text of both files."""

	def __init__(self) -> None:
		self.tables: list[str] = []
		self.constants: list[str] = []

	def constant(self, name: str, value: float, comment: str) -> None:
		self.constants.append(f"/* {comment} */\n#define {name} {literal(value)}\n")

	def dd_table(self, name: str, pairs, comment: str) -> None:
		lines = [f"/*\n * {comment}\n */\nconst struct libm_dd {name}[{len(pairs)}] = {{"]
		for high, low in pairs:
			lines.append(f"\t{{ {literal(high)}, {literal(low)} }},")
		lines.append("};\n")
		self.tables.append("\n".join(lines))

	def word_table(self, name: str, values, comment: str) -> None:
		lines = [f"/*\n * {comment}\n */\nconst uint32_t {name}[{len(values)}] = {{"]
		for index in range(0, len(values), 4):
			chunk = ", ".join(f"UINT32_C(0x{value:08x})" for value in values[index:index + 4])
			lines.append(f"\t{chunk},")
		lines.append("};\n")
		self.tables.append("\n".join(lines))

	def double_table(self, name: str, values, comment: str) -> None:
		lines = [f"/*\n * {comment}\n */\nconst double {name}[{len(values)}] = {{"]
		for value in values:
			lines.append(f"\t{literal(value)},")
		lines.append("};\n")
		self.tables.append("\n".join(lines))


def exp_section(out: Output) -> None:
	"""The exponential: 2^(j/128) and the splitting of ln 2 / 128."""
	pairs = [dd(mpf(2) ** (mpf(j) / 128)) for j in range(128)]
	out.dd_table("__libm_exp_table", pairs,
		"2^(j/128) for j = 0 .. 127, as double-doubles (the exponential).")
	step = mpmath.log(2) / 128
	first = truncate_bits(step, 35)
	second = nearest(step - mpf(first))
	third = nearest(step - mpf(first) - mpf(second))
	assert significant_bits(first) <= 35
	out.constant("LIBM_EXP_INVERSE_STEP", nearest(128 / mpmath.log(2)),
		"128 / ln 2: the number of table steps in one unit of x.")
	out.constant("LIBM_EXP_STEP_1", first,
		"ln 2 / 128 to 35 bits, so k times it is exact for |k| < 2^18.")
	out.constant("LIBM_EXP_STEP_2", second, "The next 53 bits of ln 2 / 128.")
	out.constant("LIBM_EXP_STEP_3", third, "The next 53 bits of ln 2 / 128.")
	out.constant("LIBM_ROUNDING_SHIFTER", 6755399441055744.0,
		"1.5 * 2^52: adding and subtracting it rounds a double below 2^51 to an integer.")
	coefficients = [nearest(mpf(1) / mpmath.factorial(n)) for n in range(3, 8)]
	out.double_table("__libm_exp_coefficients", coefficients,
		"1/n! for n = 3 .. 7: the Taylor coefficients of exp(r) after 1 + r + r^2/2.")


def log_section(out: Output) -> None:
	"""The logarithm: reciprocals of interval centres and their logarithms.

	The reduced significand m lies in [OFFSET, 2 * OFFSET) in encoding
	space, OFFSET = 0x3fe6b00000000000 (about 0.709).  Its encoding minus
	OFFSET, shifted right by 45, selects one of 128 intervals; interval 74 is
	centred on 1 and uses c = 1, so log x near 1 has no cancellation between
	the table and the series.
	"""
	offset = 0x3FE6B00000000000
	inverses = []
	logs = []
	worst = mpf(0)
	for index in range(128):
		low = bits_double(offset + index * (1 << 45))
		high = bits_double(offset + (index + 1) * (1 << 45))
		if index == 74:
			assert low < 1.0 < high
			inverse = 1.0
		else:
			centre = (mpf(low) + mpf(high)) / 2
			inverse = nearest(1 / centre)
		for end in (low, high):
			worst = max(worst, abs(mpf(end) * mpf(inverse) - 1))
		inverses.append(inverse)
		logs.append(dd(-mpmath.log(mpf(inverse))))
	assert worst < mpf(2) ** -7.99, worst
	out.double_table("__libm_log_inverse", inverses,
		"1/c for the 128 intervals of the reduced significand (the logarithm).")
	out.dd_table("__libm_log_table", logs,
		"-log(c) for the same intervals, as double-doubles.")
	coefficients = [nearest(mpf((-1) ** (n + 1)) / n) for n in range(4, 12)]
	out.double_table("__libm_log_coefficients", coefficients,
		"(-1)^(n+1)/n for n = 4 .. 11: the Taylor coefficients of log(1+r) after r - r^2/2 + r^3/3.")
	ln2 = mpmath.log(2)
	high = truncate_bits(ln2, 42)
	assert significant_bits(high) <= 42
	out.constant("LIBM_LN2_SPLIT_HIGH", high,
		"ln 2 to 42 bits, so e times it is exact for |e| < 2^11.")
	out.constant("LIBM_LN2_SPLIT_LOW", nearest(ln2 - mpf(high)), "The next 53 bits of ln 2.")
	high, low = dd(ln2)
	out.constant("LIBM_LN2_HIGH", high, "ln 2 as a double-double: the high part.")
	out.constant("LIBM_LN2_LOW", low, "ln 2 as a double-double: the low part.")
	high, low = dd(1 / ln2)
	out.constant("LIBM_INVERSE_LN2_HIGH", high, "1 / ln 2 as a double-double: the high part.")
	out.constant("LIBM_INVERSE_LN2_LOW", low, "1 / ln 2 as a double-double: the low part.")
	high, low = dd(1 / mpmath.log(10))
	out.constant("LIBM_INVERSE_LN10_HIGH", high, "1 / ln 10 as a double-double: the high part.")
	out.constant("LIBM_INVERSE_LN10_LOW", low, "1 / ln 10 as a double-double: the low part.")
	high, low = dd(mpf(1) / 3)
	out.constant("LIBM_THIRD_HIGH", high, "1/3 as a double-double: the high part.")
	out.constant("LIBM_THIRD_LOW", low, "1/3 as a double-double: the low part.")
	print(f"log: largest |r| = 2^{float(mpmath.log(worst, 2)):.3f}")


def trig_section(out: Output) -> None:
	"""The trigonometric functions: the reduction and the tables of i/64."""
	pi = mpmath.pi
	half_pi = pi / 2
	pieces = []
	rest = half_pi
	for bits in (33, 33, 33):
		piece = truncate_bits(rest, bits)
		assert significant_bits(piece) <= bits
		pieces.append(piece)
		rest = rest - mpf(piece)
	pieces.append(nearest(rest))
	for index, piece in enumerate(pieces):
		comment = "pi/2 to 33 bits, so k times it is exact for k < 2^20." if index == 0 else \
			("The next 33 bits of pi/2." if index < 3 else "The next 53 bits of pi/2.")
		out.constant(f"LIBM_HALF_PI_{index + 1}", piece, comment)
	out.constant("LIBM_TWO_OVER_PI", nearest(2 / pi), "2/pi, which counts the quarter turns in x.")
	high, low = dd(half_pi)
	out.constant("LIBM_HALF_PI_HIGH", high, "pi/2 as a double-double: the high part.")
	out.constant("LIBM_HALF_PI_LOW", low, "pi/2 as a double-double: the low part.")
	high, low = dd(pi)
	out.constant("LIBM_PI_HIGH", high, "pi as a double-double: the high part.")
	out.constant("LIBM_PI_LOW", low, "pi as a double-double: the low part.")
	out.constant("LIBM_QUARTER_PI", nearest(pi / 4), "pi/4 rounded to the nearest double.")
	out.constant("LIBM_THREE_QUARTER_PI", nearest(3 * pi / 4), "3pi/4 rounded to the nearest double.")
	# The bits of 2/pi after the binary point, 32 to a word, most significant first.
	words = []
	with mpmath.workprec(1400):
		value = 2 / mpmath.pi
		for _ in range(40):
			value *= 2 ** 32
			word = int(mpmath.floor(value))
			words.append(word)
			value -= word
	out.word_table("__libm_two_over_pi_bits", words,
		"The first 1280 bits of 2/pi after the binary point, 32 to a word, most significant first (Payne-Hanek reduction).")
	pairs = []
	for index in range(52):
		point = mpf(index) / 64
		pairs.append(dd(mpmath.sin(point)))
		pairs.append(dd(mpmath.cos(point)))
	out.dd_table("__libm_sin_cos_table", pairs,
		"sin(i/64) and cos(i/64) for i = 0 .. 51, as double-doubles, interleaved.")
	sine = [nearest(mpf((-1) ** n) / mpmath.factorial(2 * n + 1)) for n in range(1, 5)]
	out.double_table("__libm_sin_coefficients", sine,
		"(-1)^n/(2n+1)! for n = 1 .. 4: the Taylor coefficients of sin(d) after d.")
	cosine = [nearest(mpf((-1) ** n) / mpmath.factorial(2 * n)) for n in range(1, 5)]
	out.double_table("__libm_cos_coefficients", cosine,
		"(-1)^n/(2n)! for n = 1 .. 4: the Taylor coefficients of cos(d) - 1.")
	pairs = [dd(mpmath.atan(mpf(index) / 64)) for index in range(65)]
	out.dd_table("__libm_atan_table", pairs,
		"atan(i/64) for i = 0 .. 64, as double-doubles.")
	arctangent = [nearest(mpf((-1) ** n) / (2 * n + 1)) for n in range(1, 7)]
	out.double_table("__libm_atan_coefficients", arctangent,
		"(-1)^n/(2n+1) for n = 1 .. 6: the Taylor coefficients of atan(t) after t.")


def gamma_section(out: Output) -> None:
	"""The gamma functions: Stirling's series and the series near 1 and 2."""
	stirling = []
	for k in range(1, 13):
		stirling.append(nearest(mpmath.bernoulli(2 * k) / (2 * k * (2 * k - 1))))
	out.double_table("__libm_stirling_coefficients", stirling,
		"B(2k) / (2k (2k - 1)) for k = 1 .. 12: the terms of Stirling's series for log gamma, over x^(2k-1).")
	near_one = [nearest(mpf((-1) ** k) * mpmath.zeta(k) / k) for k in range(2, 10)]
	out.double_table("__libm_lgamma_one_coefficients", near_one,
		"(-1)^k zeta(k)/k for k = 2 .. 9: log gamma(1 + e) = -gamma e + the sum of these times e^k.")
	near_two = [nearest(mpf((-1) ** k) * (mpmath.zeta(k) - 1) / k) for k in range(2, 10)]
	out.double_table("__libm_lgamma_two_coefficients", near_two,
		"(-1)^k (zeta(k) - 1)/k for k = 2 .. 9: log gamma(2 + e) = (1 - gamma) e + the sum of these times e^k.")
	high, low = dd(mpf(1) / 12)
	out.constant("LIBM_ONE_TWELFTH_HIGH", high, "1/12, the first coefficient of Stirling's series, as a double-double: the high part.")
	out.constant("LIBM_ONE_TWELFTH_LOW", low, "1/12 as a double-double: the low part.")
	high, low = dd(mpmath.log(2 * mpmath.pi) / 2)
	out.constant("LIBM_HALF_LOG_TWO_PI_HIGH", high, "log(2 pi) / 2 as a double-double: the high part.")
	out.constant("LIBM_HALF_LOG_TWO_PI_LOW", low, "log(2 pi) / 2 as a double-double: the low part.")
	high, low = dd(mpmath.log(mpmath.pi))
	out.constant("LIBM_LOG_PI_HIGH", high, "log pi as a double-double: the high part.")
	out.constant("LIBM_LOG_PI_LOW", low, "log pi as a double-double: the low part.")
	high, low = dd(mpmath.euler)
	out.constant("LIBM_EULER_HIGH", high, "Euler's constant gamma as a double-double: the high part.")
	out.constant("LIBM_EULER_LOW", low, "Euler's constant gamma as a double-double: the low part.")
	high, low = dd(1 - mpmath.euler)
	out.constant("LIBM_ONE_MINUS_EULER_HIGH", high, "1 - gamma as a double-double: the high part.")
	out.constant("LIBM_ONE_MINUS_EULER_LOW", low, "1 - gamma as a double-double: the low part.")


# The intervals of x on which erfc(x) exp(x^2) is approximated: the centre,
# the half-width (a power of two, so t = (x - centre) / half-width is exact)
# and the degree, the smallest for which sollya's fpminimax reached a
# relative error below 2^-60 (searched once, 2026-09-28).
ERFC_INTERVALS = [
	(0.75, 0.25, 14), (1.25, 0.25, 13), (1.75, 0.25, 13), (2.25, 0.25, 12),
	(2.75, 0.25, 12), (3.5, 0.5, 14), (4.5, 0.5, 13), (5.5, 0.5, 13),
	(7.0, 1.0, 15), (9.0, 1.0, 14), (12.0, 2.0, 16), (16.0, 2.0, 15),
	(20.0, 2.0, 14), (24.0, 2.0, 13), (27.0, 1.0, 10),
]

# The degree in z = x^2 of erf(x)/x on [0, 1] (relative error 2^-68).
ERF_DEGREE = 13


def parse_sollya_number(text: str) -> Fraction:
	"""An exact value from sollya's hexadecimal display (0x1.8p-3)."""
	match = re.fullmatch(r"(-?)0x([0-9a-f]+)(?:\.([0-9a-f]*))?p([-+]?[0-9]+)", text.strip())
	if match is None:
		if text.strip() in ("0", "-0"):
			return Fraction(0)
		raise ValueError(text)
	sign, whole, fraction, exponent = match.groups()
	fraction = fraction or ""
	mantissa = int(whole + fraction, 16)
	value = Fraction(mantissa) * Fraction(2) ** (int(exponent) - 4 * len(fraction))
	return -value if sign else value


def run_sollya(script: str) -> dict[str, list[Fraction]]:
	"""Runs a sollya script whose lines "COEF tag index value" are collected."""
	with tempfile.NamedTemporaryFile("w", suffix=".sollya", delete=False) as handle:
		handle.write(script)
		path = handle.name
	try:
		output = subprocess.run(["sollya", path], check=True, capture_output=True, text=True).stdout
	finally:
		os.unlink(path)
	result: dict[str, list[Fraction]] = {}
	for line in output.splitlines():
		fields = line.split()
		if len(fields) == 4 and fields[0] == "COEF":
			result.setdefault(fields[1], []).append(parse_sollya_number(fields[3]))
	return result


def erf_section(out: Output) -> None:
	"""erf and erfc: minimax polynomials from sollya's fpminimax.

	erf(x)/x is a polynomial in z = x^2 on [0, 1].  erfc(x) exp(x^2) is a
	polynomial in t = (x - c)/h on each interval of ERFC_INTERVALS.  The
	first two coefficients are double-doubles (fpminimax's DD format), the
	others doubles.  The method is that of Brisebarre and Chevillard,
	"Efficient polynomial L-infinity approximations" (2007), as sollya
	implements it; the functions approximated are erf and erfc themselves.
	"""
	lines = ["prec = 400;", "display = hexadecimal;", "verbosity = 0;"]
	lines.append("procedure emit(tag, p, n) { var i; for i from 0 to n do print(\"COEF\", tag, i, coeff(p, i)); };")
	lines.append(f"p = fpminimax(erf(sqrt(x))/sqrt(x), {ERF_DEGREE}, [|DD, DD, D...|], [2^-100;1], relative, floating);")
	lines.append(f"emit(\"erf\", p, {ERF_DEGREE});")
	for index, (centre, half, degree) in enumerate(ERFC_INTERVALS):
		lines.append(f"f = erfc({centre!r} + {half!r}*x)*exp(({centre!r} + {half!r}*x)^2);")
		lines.append(f"p = fpminimax(f, {degree}, [|DD, DD, D...|], [-1;1], relative, floating);")
		lines.append(f"emit(\"erfc{index}\", p, {degree});")
	lines.append("quit;")
	coefficients = run_sollya("\n".join(lines) + "\n")

	def flatten(values: list[Fraction]) -> list[float]:
		flat = []
		for position, value in enumerate(values):
			high = float(value)
			if position < 2:
				low = float(value - Fraction(high))
				flat += [high, low]
			else:
				assert Fraction(high) == value, "a D coefficient is not a double"
				flat.append(high)
		return flat

	out.double_table("__libm_erf_coefficients", flatten(coefficients["erf"]),
		f"erf(x)/x as a polynomial of degree {ERF_DEGREE} in x^2 on [0, 1] (fpminimax): c0 and c1 as high and low parts, then c2 ..")
	flat = []
	rows = []
	for index, (centre, half, degree) in enumerate(ERFC_INTERVALS):
		rows.append((centre + half, centre, 1.0 / half, len(flat), degree))
		flat += flatten(coefficients[f"erfc{index}"])
	out.double_table("__libm_erfc_coefficients", flat,
		"erfc(x) exp(x^2) as polynomials in t = (x - centre)/h, one per interval of __libm_erfc_intervals (fpminimax): c0 and c1 as high and low parts, then c2 ..")
	text = [f"/*\n * The intervals of the erfc polynomials: upper end, centre, 1/h, first coefficient, degree.\n */\nconst struct libm_polynomial_interval __libm_erfc_intervals[{len(rows)}] = {{"]
	for upper, centre, inverse, first, degree in rows:
		text.append(f"\t{{ {literal(upper)}, {literal(centre)}, {literal(inverse)}, {first}, {degree} }},")
	text.append("};\n")
	out.tables.append("\n".join(text))
	high, low = dd(2 / mpmath.sqrt(mpmath.pi))
	out.constant("LIBM_TWO_OVER_SQRT_PI_HIGH", high, "2/sqrt(pi) as a double-double: the high part.")
	out.constant("LIBM_TWO_OVER_SQRT_PI_LOW", low, "2/sqrt(pi) as a double-double: the low part.")


def main() -> None:
	out = Output()
	exp_section(out)
	log_section(out)
	trig_section(out)
	gamma_section(out)
	erf_section(out)
	tables = HEADER + """/*
 * The tables of the mathematical library.
 *
 * Generated by src/libc/math/gen/gen-tables.py; do not edit.  The values
 * are elementary functions at chosen points, computed at 256 bits with
 * mpmath and rounded to the nearest double.
 */

#include "src/libc/math/math-internal.h"

""" + "\n".join(out.tables)
	constants = HEADER + """/*
 * The constants of the mathematical library.
 *
 * Generated by src/libc/math/gen/gen-tables.py; do not edit.
 */

#ifndef LIBC_MATH_CONSTANTS_H
#define LIBC_MATH_CONSTANTS_H

""" + "\n".join(out.constants) + "\n#endif\n"
	with open(OUT_TABLES, "w") as handle:
		handle.write(tables.rstrip("\n") + "\n")
	with open(OUT_CONSTANTS, "w") as handle:
		handle.write(constants)


if __name__ == "__main__":
	main()
