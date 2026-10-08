#!/bin/sh
# ws101-p002: the i915 compiler's compute shaders on the host.
#
# 1. Compiles the GLSL of shaders/ with glslc (Vulkan 1.0 SPIR-V), and the shared-memory and barrier shaders of the vkcs
#    scenario and its refusals (src/drivers/gpu/i915/tests/render/compute-shaders/, ws101-p006).
# 2. compute-dump.c parses and compiles each module as a compute shader: the scoreboard must be sound, every
#    atomic's reply bit must agree with its reply length, the kernel must end at the thread spawner; the modules of
#    shaders/refuse/ must be refused.
# 3. Mesa's brw_disasm must accept every instruction of each kernel and brw_asm must assemble the listing back into
#    the same bytes.
# 4. compute-lower.c runs each module's IR on a small interpreter, dispatch by dispatch, and compares the buffers with
#    what C computes on its own.
# 5. executor-test.c (ws101-p003) creates compute pipelines and records dispatches through the executor's wire.
# 6. compute-batch-test.c (ws101-p004) writes dispatches' slots and commands (render/compute.c) and checks the slots;
#    genxml-check.py decodes the commands and the interface descriptors with Mesa's genxml (gen120.xml) and checks
#    their fields.
#
#   plan/ws101/tests/host/run.sh        (BRW_TOOLS: a Mesa 25.0.7 build's src/intel/compiler, default below;
#                                        GENXML: a Mesa 25.0.7 tree's src/intel/genxml, default below)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -u
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
here=$repo/plan/ws101/tests/host
tools=${BRW_TOOLS:-$repo/build/mesa-tools/build-asm/src/intel/compiler}
genxml=${GENXML:-$repo/build/mesa-tools/mesa-25.0.7/src/intel/genxml}
work=$(mktemp -d "${TMPDIR:-/tmp}/ws101-host.XXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM
status=0

# Mesa's tools must be there for the round trip and the genxml decode: without them those checks are reported as not
# run (the other checks still run, and the result says so), instead of failing every kernel as a mismatch.
roundtrip=1
if [ ! -x "$tools/brw_disasm" ] || [ ! -x "$tools/brw_asm" ]; then
	roundtrip=0
	echo "NOTE: no brw_disasm/brw_asm under $tools (plan/ws101/tests/host/mesa-tools.sh builds them, or set BRW_TOOLS);" \
		"the disassembly round trip is NOT RUN"
fi
decode=1
if [ ! -f "$genxml/gen120.xml" ]; then
	decode=0
	echo "NOTE: no gen120.xml under $genxml (plan/ws101/tests/host/mesa-tools.sh unpacks it, or set GENXML);" \
		"the genxml decode of the commands is NOT RUN"
fi

# The tools: the dumper and the interpreter, built from the compiler's sources.
cc -std=gnu99 -O0 -Wall -Wextra -Wno-unused-function -I"$repo" -I"$repo/include" -DHAL_ARCH_AMD64 \
	-o "$work/dump" "$here/compute-dump.c" -lm || exit 1
cc -std=gnu99 -O0 -Wall -Wextra -Wno-unused-function -I"$repo" -I"$repo/include" -DHAL_ARCH_AMD64 \
	-o "$work/lower" "$here/compute-lower.c" -lm || exit 1

# The modules.
mkdir -p "$work/refuse"
for source in "$here"/shaders/*.comp "$here"/shaders/refuse/*.comp; do
	relative=${source#"$here"/shaders/}
	glslc --target-env=vulkan1.0 -fshader-stage=compute -o "$work/${relative%.comp}.spv" "$source" || {
		echo "$relative: FAIL glslc"; status=1; }
done
vkcs=$repo/src/drivers/gpu/i915/tests/render/compute-shaders
for name in shared reduce scan oddbar atomsh; do
	glslc --target-env=vulkan1.0 -fshader-stage=compute -o "$work/$name.spv" "$vkcs/$name.comp" || {
		echo "$name: FAIL glslc"; status=1; }
done
for source in "$vkcs"/refuse/*.comp; do
	name=$(basename "$source" .comp)
	glslc --target-env=vulkan1.0 -fshader-stage=compute -o "$work/refuse/vkcs-$name.spv" "$source" || {
		echo "vkcs refuse/$name: FAIL glslc"; status=1; }
done

# Every module compiles, and its kernel passes Mesa's disassembler and assembler.
for spv in "$work"/*.spv; do
	name=$(basename "$spv" .spv)
	"$work/dump" "$spv" "$work/$name.bin" > "$work/$name.dump" 2>&1 || {
		cat "$work/$name.dump"; echo "$name: FAIL"; status=1; continue; }
	grep -q 'descriptors agree' "$work/$name.dump" || { echo "$name: FAIL descriptors"; status=1; continue; }
	if [ $roundtrip -eq 0 ]; then
		echo "$name: compiled (round trip not run)"
		grep 'scoreboard\|sends,' "$work/$name.dump" | sed "s|^[^:]*:|$name:|"
		continue
	fi
	"$tools/brw_disasm" --gen=adl --input-path="$work/$name.bin" > "$work/$name.asm" 2>&1
	if grep -q 'ERROR\|illegal' "$work/$name.asm"; then
		echo "$name: FAIL the disassembler rejects an instruction"; grep -B1 'ERROR\|illegal' "$work/$name.asm" | head
		status=1; continue
	fi
	"$tools/brw_asm" --gen=adl -o "$work/$name.re" "$work/$name.asm" > /dev/null 2>&1
	cmp -s "$work/$name.bin" "$work/$name.re" || { echo "$name: FAIL re-assembled bytes differ"; status=1; continue; }
	echo "$name: $(grep -c '' "$work/$name.asm") listing lines; disassembled and re-assembled to the same bytes"
	grep 'scoreboard\|sends,' "$work/$name.dump" | sed "s|^[^:]*:|$name:|"
done

# The modules that must be refused are.
for spv in "$work"/refuse/*.spv; do
	"$work/dump" -refuse "$spv" > "$work/refuse.out" 2>&1 || status=1
	sed "s|^.*/refuse/|refuse/|" "$work/refuse.out"
done

# The IR computes what C computes.
"$work/lower" "$work" || status=1

# ws101-p003: the executor's compute pipelines and the recording of dispatches, through the wire, on the stand-ins of
# plan/ws031/tests/i915-vk-render-stubs.inc (the executor's sources as run-vk-host-tests.sh links them, command.c
# inside the test itself).
driver=$repo/src/drivers/gpu/i915
executor=""
for part in codec object dispatch transport instance vulkan fence objects reply memory image descriptor pipeline \
    pipeline-prepare render-pass sync state batch math forget video video-mfx video-h264-tables; do
	executor="$executor $driver/render/$part.c"
done
for part in spirv compile eu; do
	executor="$executor $driver/compiler/$part.c"
done
cp "$repo/userland/tests/vkdemo/shaders/cuboid.vert.spv" "$repo/userland/tests/vkdemo/shaders/cuboid.frag.spv" "$work/"
cc -std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO="\"$repo\"" \
	-I"$repo/include" -I"$repo" -idirafter "$repo/include/libc" -o "$work/executor" "$here/executor-test.c" $executor -lm || exit 1
"$work/executor" "$work" || status=1

# ws101-p004: the dispatch's slot and commands, compute.c inside the test itself, on the same stand-ins.
cc -std=gnu11 -Wall -Wextra -Werror -Wdeclaration-after-statement -DKERN_USER_ABI_LP64 -DVK_REPO="\"$repo\"" \
	-I"$repo/include" -I"$repo" -idirafter "$repo/include/libc" -o "$work/batch" "$here/compute-batch-test.c" \
	$executor "$driver/render/command.c" -lm || exit 1
"$work/batch" "$work" "$work/dispatch" || status=1
if [ $decode -eq 1 ]; then
	for name in add ids indirect reduce; do
		python3 "$here/genxml-check.py" "$genxml" "$work/dispatch-$name" || status=1
	done
fi

missing=""
[ $roundtrip -eq 0 ] && missing="the brw_disasm/brw_asm round trip"
[ $decode -eq 0 ] && missing="${missing:+$missing and }the genxml decode"
if [ $status -ne 0 ]; then
	echo "ws101 host test FAIL"
elif [ -n "$missing" ]; then
	echo "ws101 host test PASS (without $missing: no Mesa tools)"
else
	echo "ws101 host test PASS"
fi
exit $status
