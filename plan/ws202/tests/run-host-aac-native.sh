#!/bin/sh
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# Real native PCM, packet boundaries, profile refusal and continuous resampling; no GPU/guest required.
set -eu
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
cd "$repo"
. "$repo/plan/tools/fresh-out.sh"
fresh_out "$repo/build/tmp/ws202-aac-native-check"
work=$fresh_dir
sh plan/ws202/tests/make-streams.sh > "$work/streams-path"
streams=$(cat "$work/streams-path")
compiler=${CC:-cc}
sources='userland/desktop/libmedia/bits.c userland/desktop/libmedia/aac-input.c userland/desktop/libmedia/aac-codec.c userland/desktop/libmedia/aac-huffman.c userland/desktop/libmedia/aac-bands.c userland/desktop/libmedia/aac-frame.c userland/desktop/libmedia/aac-synth.c userland/desktop/libmedia/aac.c userland/desktop/libmedia/sound.c'
for mode in plain sanitize; do
    extra=''
    if [ "$mode" = sanitize ]; then
        extra='-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer'
    fi
    for suite in native backend reject; do
        "$compiler" -std=c89 -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g $extra -pthread -I. \
            "plan/ws202/tests/host-aac-$suite.c" $sources -lm -o "$work/$suite-$mode"
    done
    for name in stereo mono low transient intensity; do
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$work/native-$mode" "$streams/aac-$name.aac" "$work/$name-$mode.f32"
    done
    ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
        "$work/reject-$mode" "$streams/aac-stereo.aac"
    for raw in 0 1; do
        ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1 \
            "$work/backend-$mode" "$streams/aac-intensity.aac" "$work/backend-$raw-$mode.s16" 48000 "$raw"
    done
    cmp "$work/backend-0-$mode.s16" "$work/backend-1-$mode.s16"
done
"$compiler" -std=c89 -Wall -Wextra -Werror -Wno-long-long -pedantic -O1 -g -I. \
    plan/ws202/tests/host-sound.c userland/desktop/libmedia/sound.c -lm -o "$work/sound"
"$work/sound" 8000 997 "$work/sinc-8k.s16"
"$work/sound" 44100 997 "$work/sinc-44k.s16"
"$work/sound" 96000 997 "$work/sinc-96k.s16"
"$work/sound" 96000 30000 "$work/sinc-alias.s16"
python3 - "$streams" "$work" <<'PY'
import array
import math
import pathlib
import sys

def samples(path, code):
    result = array.array(code)
    result.frombytes(path.read_bytes())
    return result

streams, work = map(pathlib.Path, sys.argv[1:])
for name in ['stereo', 'mono', 'low', 'transient', 'intensity']:
    ref = samples(streams / ('aac-' + name + '.f32'), 'f')
    native = samples(work / (name + '-plain.f32'), 'f')
    assert len(ref) == len(native), name
    signal = sum(x*x for x in ref)
    error = sum((x-y)**2 for x,y in zip(ref, native))
    snr = 10*math.log10(signal/max(error, 1e-99))
    if name in ['transient', 'intensity']:
        assert max(abs(x-y) for x,y in zip(ref,native)) <= 2**-14, name
        assert math.sqrt(error/len(ref)) <= 2**-17, name
    else:
        # PNS random vectors differ; ordinary transforms are compared independently above.
        assert snr >= 70, (name, snr)
    print('AAC PCM comparison', name, 'SNR %.2f dB' % snr)
for name in ['8k','44k','96k','alias']:
    data = samples(work / ('sinc-' + name + '.s16'), 'h')
    values = data[512:-512:2]
    rms = math.sqrt(sum((x/32768)**2 for x in values)/len(values))
    if name == 'alias':
        assert rms < 3e-5, rms
    else:
        assert abs(rms/(0.5/math.sqrt(2))-1) < 0.001, (name, rms)
    print('PCM resampling', name, 'RMS %.8f' % rms)
PY
printf '%s\n' "WS202 native AAC host PASS; artifacts $work"
