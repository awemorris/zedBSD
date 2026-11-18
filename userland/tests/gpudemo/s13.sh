#!/bin/sh
# The system's GPU compute demonstration (WS101, the scene S13 of the demonstration's script): Noct runs the same program
# on the CPU and then on the GPU and the two times are shown side by side.  The program (mix.nct) hashes every
# element of an array of N integers (4,000,000 by default) with 32 operations each, in a loop Noct parallelizes
# by itself: with --gpu it becomes an OpenGL ES compute shader (libGLESv2 on the i915's Vulkan).  Both runs check
# their results against the same function computed on the CPU and print a checksum, which must be equal.
#
#   sh /usr/share/gpudemo/s13.sh [N]
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
n=${1:-4000000}
dir=$(dirname -- "$0")
noct="noct -O2 -j --gc-tenure-size=100000000"
cpu=/tmp/gpudemo-cpu.log
gpu=/tmp/gpudemo-gpu.log

# The system's name as os-release gives it (the file is in the shell's syntax).
name=$(. /etc/os-release 2>/dev/null; echo "$NAME")
echo "${name:-zedBSD} GPU compute: $n integers, 32 operations each, in Noct"
echo "CPU run..."
$noct "$dir/mix.nct" "$n" 6 > "$cpu" 2>&1 || { cat "$cpu"; echo "The CPU run failed."; exit 1; }
echo "GPU run..."
ZEDBSD_GLES_COMPUTE_TRACE=1 $noct --gpu "$dir/mix.nct" "$n" 6 > "$gpu" 2>&1 || { cat "$gpu"; echo "The GPU run failed."; exit 1; }

# The medians (after the first call), the checks and the checksums.
cpu_ms=$(sed -n 's/^MIX first_ms=[0-9]* median_ms=\([0-9]*\).*/\1/p' "$cpu")
gpu_ms=$(sed -n 's/^MIX first_ms=[0-9]* median_ms=\([0-9]*\).*/\1/p' "$gpu")
cpu_sum=$(grep '^MIX checksum' "$cpu")
gpu_sum=$(grep '^MIX checksum' "$gpu")
dispatches=$(grep -c '^gles: compute dispatch' "$gpu")
echo "CPU: ${cpu_ms} ms a run"
echo "GPU: ${gpu_ms} ms a run ($dispatches kernels ran on the GPU)"
# The faster of the two, and by how much (a run under 1 ms counts as 1 ms).
if [ -n "$cpu_ms" ] && [ -n "$gpu_ms" ]; then
	awk -v c="$cpu_ms" -v g="$gpu_ms" 'BEGIN {
		if (c < 1) c = 1
		if (g < 1) g = 1
		if (g <= c) printf "The GPU is %.1f times as fast as the CPU.\n", c / g
		else printf "The CPU is %.1f times as fast as the GPU.\n", g / c
	}'
fi
if grep -q '^MIX check=[0-9]* wrong=0' "$cpu" && grep -q '^MIX check=[0-9]* wrong=0' "$gpu" && [ "$cpu_sum" = "$gpu_sum" ]; then
	echo "The results are the same on the CPU and the GPU."
	exit 0
fi
echo "The results differ."
exit 1
