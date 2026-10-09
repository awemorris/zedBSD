#!/usr/bin/env python3
"""Derives the HVS coefficients independently from Mitchell-Netravali (1988), equation 8.

B=C=1/3, x=(31-2*i)/16, Q8 nearest rounding. Correct each mirrored
polyphase pair at its largest weight so a constant input stays constant.
The kernel stores only these mathematical results and does no floating point.
Primary source: https://www.cs.utexas.edu/~fussell/courses/cs384g-fall2013/lectures/mitchell/Mitchell.pdf
"""
import re
from fractions import Fraction
from pathlib import Path


def reconstruct(x):
    """Evaluates the two cubic pieces exactly after substituting B=C=1/3."""
    if x < 1:
        return (21 * x**3 - 36 * x**2 + 16) / 18
    if x < 2:
        return (-7 * x**3 + 36 * x**2 - 60 * x + 32) / 18
    return Fraction(0)


def nearest(value):
    """Rounds rational Q8 samples to nearest with symmetric ties away from zero."""
    numerator = value.numerator
    denominator = value.denominator
    rounded = (2 * abs(numerator) + denominator) // (2 * denominator)
    return -rounded if numerator < 0 else rounded


half = [nearest(256 * reconstruct(Fraction(31 - 2 * i, 16))) for i in range(16)]
full = half + list(reversed(half))
for phase in range(4):
    positions = list(range(phase, 32, 8))
    selected = max(positions, key=lambda index: full[index])
    correction = 256 - sum(full[index] for index in positions)
    full[selected] += correction
    full[31 - selected] += correction
assert all(sum(full[phase::8]) == 256 for phase in range(8))
assert full == list(reversed(full))
assert all(-256 <= value < 256 for value in full)
expected = full[:16] + [full[15], 0]
source = Path('src/drivers/gpu/bcm2711/display-program.c').read_text()
array = re.search(r'program_filter_samples\[18\]\s*=\s*\{([^}]+)\}', source).group(1)
actual = [int(value) for value in re.findall(r'-?\d+', array)]
assert actual == expected, (actual, expected)
print('WS141 independent rational reconstruction/Q8/symmetry/eight-phase DC/own table: PASS')
