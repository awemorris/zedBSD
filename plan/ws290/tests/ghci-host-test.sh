#!/bin/sh
# Host test of the GHCI 1.0 driver (WS290): the words test (every command encoder,
# the records, the fence, ring, capability list and sizes; ghci-words-host-test.c)
# and the reference test of the layouts against amaterrace-gpu's include/ghci_cmd.h
# (ghci-ref-host-test.c + ghci-ref-layout.c). The reference header is written to a
# new directory for the run only; without amaterrace-gpu the reference test FAILS.
# Then the self-test of the GPU model and the kernel stubs (ghci-model-host-test.c with
# ghci-model.c and ghci-stubs.c); the stubs' registration checks are gpu_ops_validate,
# taken out of src/drivers/gpu/gpu.c for the run so that they stay the same as gpu.c's.
# Then the driver's core and its display against the model (ghci-driver-host-test.c), a grep of the
# files the kernel builds for the names of the interface before GHCI 1.0, the compiler,
# the Vulkan executor against the model (ghci-vk-host-test.c), its pipelines and descriptors
# (ghci-pipeline-host-test.c, with shaders compiled by glslc), the execution of recorded commands, and
# the driver's own transfer programs run on the GSI1 model (ghci-meta-host-test.c, ghci-meta-run.py).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# usage: plan/ws290/tests/ghci-host-test.sh   (from anywhere)
# Environment: GHCI_SPEC_REPO (default ~/amaterrace-gpu), GHCI_SPEC_REV (default 288c459).
# Needs cc, git and glslc. Output goes to a new directory below build/tmp; nothing is removed.
set -eu
repo=$(cd "$(dirname "$0")/../../.." && pwd)
tests="$repo/plan/ws290/tests"
ghci="$repo/src/drivers/gpu/ghci"
spec_repo=${GHCI_SPEC_REPO:-$HOME/amaterrace-gpu}
spec_rev=${GHCI_SPEC_REV:-288c459}
mkdir -p "$repo/build/tmp"
out=$(mktemp -d "$repo/build/tmp/ghci290-host.XXXXXX")
export TMPDIR="$out"
flags="-std=gnu11 -O1 -g -DKERN_USER_ABI_LP64 -I$repo/include -I$repo/src -idirafter $repo/libc/include \
-fsanitize=address,undefined -fno-sanitize=alignment -Wall -Wextra -Werror -Wdeclaration-after-statement"
status=0

cc $flags -o "$out/ghci-words-host-test" "$tests/ghci-words-host-test.c" "$ghci/cmd.c" "$ghci/caps.c" "$ghci/check.c"
timeout 120 "$out/ghci-words-host-test" || status=1

mkdir -p "$out/ref"
if git -C "$spec_repo" show "$spec_rev:include/ghci_cmd.h" > "$out/ref/ghci_cmd.h" 2>/dev/null; then
	cc -std=gnu11 -O1 -Wall -Wextra -Werror -I"$out/ref" -c -o "$out/ghci-ref-layout.o" "$tests/ghci-ref-layout.c"
	cc $flags -o "$out/ghci-ref-host-test" "$tests/ghci-ref-host-test.c" "$out/ghci-ref-layout.o"
	timeout 60 "$out/ghci-ref-host-test" || status=1
else
	echo "FAIL ghci-ref-host-test: no $spec_rev:include/ghci_cmd.h in $spec_repo"
	status=1
fi

# The registration checks of gpu.c, as a function the stubs call.
validate="$out/gpu-ops-validate.c"
{
	printf '#include <stddef.h>\n#include "drivers/gpu/gpu.h"\n#include "drivers/gpu/gpu-scanout.h"\n#include "uapi/gpu-allocation.h"\n#include "uapi/gpu-fence.h"\n#include "uapi/gpu-job.h"\n#include "drivers/gpu/gpu-fence.h"\n#include "uapi/errno.h"\n#include "ghci-stubs.h"\n\n'
	awk 'found == 0 && /^gpu_ops_validate\($/ { found = 1; print "int"; print "stub_gpu_ops_validate("; next }
	     found == 1 { print; if ($0 ~ /^}$/) exit }' "$repo/src/drivers/gpu/gpu.c"
} > "$validate"
if grep -q '^stub_gpu_ops_validate($' "$validate"; then
	cc $flags -I"$tests" -o "$out/ghci-model-host-test" "$tests/ghci-model-host-test.c" "$tests/ghci-model.c" \
		"$tests/ghci-stubs.c" "$validate" "$ghci/caps.c" "$ghci/cmd.c"
	timeout 120 "$out/ghci-model-host-test" || status=1
else
	echo "FAIL ghci-model-host-test: gpu_ops_validate not found in src/drivers/gpu/gpu.c"
	status=1
fi

# The driver's core against the model (ghci-driver-host-test.c).
if grep -q '^stub_gpu_ops_validate($' "$validate"; then
	cc $flags -I"$tests" -o "$out/ghci-driver-host-test" "$tests/ghci-driver-host-test.c" "$tests/ghci-model.c" \
		"$tests/ghci-stubs.c" "$validate" "$ghci/ghci.c" "$ghci/cp.c" "$ghci/recover.c" "$ghci/memory.c" \
		"$ghci/vm.c" "$ghci/range.c" "$ghci/share.c" "$ghci/display.c" "$ghci/display-scan.c" "$ghci/adv7511.c" \
		"$ghci/check.c" "$ghci/caps.c" "$ghci/cmd.c"
	timeout 300 "$out/ghci-driver-host-test" || status=1
fi

# The files the kernel builds of the driver, and the headers they include, name nothing of the
# interface before GHCI 1.0 (the 128 KiB registers, the firmware, the native command, the aperture,
# the packets of class 0 to 2 and their job tags).
built=$(sed -n '/^# \[GHCI GPU driver\]/,/^endif/p' "$repo/mk/kernel.mk" | grep -o 'src/drivers/gpu/ghci/[a-z0-9/-]*\.c' || true)
headers="$ghci/ghci.h $ghci/ghci-regs.h $ghci/ghci-cmd.h $ghci/ghci-words.h"
old_names='hcr\.h|packets\.h|frame\.h|GPU_OP_GHCI|CONFIG_GHCI_NATIVE|ghci_native|ghci_packet_|ghci_blit|ghci_firmware|firmware\.c|aperture|APERTURE|GHCI_CAPS_CP|GHCI_XCAP_|GHCI_CPFW|GHCI_TAG_|job tag'
if [ -z "$built" ]; then
	echo "FAIL ghci-old-names: no GHCI sources in mk/kernel.mk"
	status=1
elif (cd "$repo" && grep -nE "$old_names" $built) || grep -nE "$old_names" $headers; then
	echo "FAIL ghci-old-names: the files above name the interface before GHCI 1.0"
	status=1
else
	echo "PASS ghci-old-names ($(echo $built | wc -w) sources, 4 headers)"
fi

# The shader compiler (gsi1-host-test.sh: the compiler's sources under the flags above, the harness,
# the reference tools of gsi1-tools/ and the model; its own new directory below build/tmp), and the
# choice of ISA and options (isa.c) compiled under the flags above.
if cc $flags -I"$repo" -Wmissing-prototypes -c -o "$out/isa.o" "$ghci/isa.c"; then
	echo "PASS ghci-isa (isa.c compiles)"
else
	echo "FAIL ghci-isa: isa.c does not compile"
	status=1
fi
timeout 900 sh "$tests/gsi1-host-test.sh" || status=1

# The Vulkan executor (vk/) with the driver's core against the model (ghci-vk-host-test.c), and the
# record codec it shares with the i915 executor. The executor's pipelines compile with the shader
# compiler and the shared SPIR-V frontend (which keeps its own declaration style).
if grep -q '^stub_gpu_ops_validate($' "$validate"; then
	vk="$ghci/vk"
	mkdir -p "$out/compiler"
	compiler_objects=""
	for source in gsi1-compile gsi1-encode gsi1-io gsi1-memory gsi1-texture gsi1-image gsi1-schedule gsi1-spill; do
		cc $flags -I"$repo" -c -o "$out/compiler/$source.o" "$ghci/compiler/$source.c"
		compiler_objects="$compiler_objects $out/compiler/$source.o"
	done
	for source in spirv optimize; do
		cc $flags -I"$repo" -Wno-declaration-after-statement -c -o "$out/compiler/$source.o" "$repo/src/drivers/gpu/compiler/$source.c"
		compiler_objects="$compiler_objects $out/compiler/$source.o"
	done
	executor="$ghci/ghci.c $ghci/cp.c $ghci/recover.c $ghci/memory.c $ghci/vm.c $ghci/range.c $ghci/share.c \
		$ghci/display.c $ghci/display-scan.c $ghci/adv7511.c \
		$ghci/check.c $ghci/caps.c $ghci/cmd.c $ghci/jobs.c $ghci/capset.c $ghci/isa.c $vk/vk-node.c $vk/vk-stream.c \
		$vk/vk-dispatch.c $vk/vk-object.c $vk/vk-reply.c $vk/vk-caps.c $vk/vk-format.c $vk/vk-device.c \
		$vk/vk-memory.c $vk/vk-command.c $vk/vk-queue.c $vk/vk-layout.c $vk/vk-pipeline.c $vk/vk-pass.c $vk/vk-execute.c $vk/vk-meta.c \
		$repo/src/drivers/gpu/i915/vk/vk-codec.c"
	cc $flags -I"$repo" -I"$tests" -o "$out/ghci-vk-host-test" "$tests/ghci-vk-host-test.c" "$tests/ghci-model.c" \
		"$tests/ghci-stubs.c" "$validate" $executor $compiler_objects -lm
	timeout 300 "$out/ghci-vk-host-test" || status=1

	# The pipelines and descriptors (ghci-pipeline-host-test.c) with the shaders of pipeline/ and the
	# compiler test's attrib60.vert and spill.comp, compiled by glslc.
	mkdir -p "$out/pipeline"
	for source in "$tests"/pipeline/*.vert "$tests"/pipeline/*.frag "$tests"/pipeline/*.comp "$tests/gsi1/attrib60.vert" \
		"$tests/gsi1/spill.comp"; do
		glslc --target-env=vulkan1.0 -o "$out/pipeline/$(basename "$source").spv" "$source"
	done
	cc $flags -I"$repo" -I"$tests" -o "$out/ghci-pipeline-host-test" "$tests/ghci-pipeline-host-test.c" "$tests/ghci-model.c" \
		"$tests/ghci-stubs.c" "$validate" $executor $compiler_objects -lm
	timeout 300 "$out/ghci-pipeline-host-test" "$out/pipeline" "$out/pipeline" || status=1

	# The execution of recorded commands (ghci-execute-host-test.c, compiled within the pipeline test for its
	# helpers): the same binary given "execute".
	timeout 300 "$out/ghci-pipeline-host-test" "$out/pipeline" "$out/pipeline" execute || status=1

	# The driver's own transfer programs (ghci-meta-host-test.c, in the same binary given "meta"): their packets,
	# and every dispatch run on the GSI1 model of gsi1-tools/ (ghci-meta-run.py), its files in a new directory.
	mkdir -p "$out/meta"
	timeout 900 "$out/ghci-pipeline-host-test" "$out/pipeline" "$out/pipeline" meta "$tests" "$tests/gsi1-tools" "$out/meta" || status=1
fi

if [ "$status" -ne 0 ]; then
	echo "FAIL ghci290-host-test ($out)"
	exit 1
fi
echo "PASS ghci290-host-test ($out)"
