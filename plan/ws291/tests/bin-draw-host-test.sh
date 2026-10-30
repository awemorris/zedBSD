#!/bin/sh
# Host test of the V3D draw packets (indexed, instanced, bases) and the attribute record's instance divisor:
# both binning encoders' sequences decoded with Mesa's v3d_packet.xml (MIT, ignored temp, pinned hash).
# Usage: sh plan/ws291/tests/bin-draw-host-test.sh [output-directory]  (from the tree root)
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
set -eu
out=${1:-build/ws291-bin-draw-host}
xml=plan/ws141/temp/mesa/src/broadcom/cle/v3d_packet.xml
mkdir -p "$out"
echo "b13f995b1a4b606a1adcb2c048ff9bca067106be14e22ce1ee86e01055094e1b  $xml" | sha256sum -c
for version in 42 71; do
	bin=src/drivers/gpu/bcm2711/native-bin.c
	[ "$version" = 71 ] && bin=src/drivers/gpu/bcm2711/native71-bin.c
	cc -std=c99 -O1 -Wall -Wextra -Werror -Iinclude -Isrc -I. \
		plan/ws291/tests/bin-draw-host-test.c "$bin" src/drivers/gpu/bcm2711/native-shader.c \
		-o "$out/bin-draw-host-test-$version"
	timeout 60 "$out/bin-draw-host-test-$version" > "$out/lines-$version"
	python3 -I plan/ws291/tests/bin-draw-host-test.py "$version" "$xml" < "$out/lines-$version"
done
