#!/bin/sh
# WS031 E-127: one hardware iteration of the libvulkan connectivity loop.
# Builds the resident image with the vkprobe service chain, runs it on the 5330 (ssh alias
# solaris10-man) and prints what the application, the executor and the GPU core said.
#
# usage: plan/ws031/tests/vkloop-hw.sh                 one run; the log lines that matter
#        plan/ws031/tests/vkloop-hw.sh oracle          the same with the first frame dumped over serial,
#                                                      rebuilt and given to the independent pixel oracle
#                                                      (plan/ws014/tests/vkdemo_oracle.py).  The dump is slow:
#                                                      the application times out after it, by design.
#        plan/ws031/tests/vkloop-hw.sh oracle 2500     the same for the frame of shader time 2500 ms (--time-ms)
#        plan/ws031/tests/vkloop-hw.sh display [ms]    E-129: on the panel (no --offscreen), the frame of shader time ms
#                                                      (default 2500) held 20 s for the camera; "display live": 12 s of animation
#        plan/ws031/tests/vkloop-hw.sh "-DFOO=1"       extra CPPFLAGS
#        plan/ws031/tests/vkloop-hw.sh "oracle -DI915_VK_REFERENCE_KERNELS=1"   (flags after the word)
#        plan/ws031/tests/vkloop-hw.sh wayland         the zwl compositor and two wltest clients (FIFO 600 frames, then mailbox
#                                                      with a swapchain recreate) in place of vkdemo; services in plan/ws031/tests/wayland/
#        plan/ws031/tests/vkloop-hw.sh mview           the model viewer (zwl + mview, services in plan/ws031/tests/mview/) in place of vkdemo;
#                                                      MVIEW_ARGS="--spin=30" adds viewer options; MVIEW_MODEL=test shows
#                                                      userland/base/mview/models/test/ (it has a blend material) in place of qs40
#                                                      MVIEW_ARGS="--shading=pixel" (or MVIEW_NO_VENUS=1) makes a CAPTURE=mview run skip
#                                                      the comparison with the p013 Venus images (i915-capture.py --no-venus);
#                                                      the six views are also on one sheet, /tmp/capture-last/sheet.png
#        CAPTURE=<vkdemo|wayland|mview> plan/ws031/tests/vkloop-hw.sh [display|wayland|mview]
#                                                      the capture build (I915_TEST_CAPTURE=y: no LCD, presents copied into guest RAM);
#                                                      plan/ws031/tests/i915-capture.py reads them over QMP on the 5330 and checks the
#                                                      scenario; images and result.json land in /tmp/capture-last/
#        plan/ws031/tests/vkloop-hw.sh test <scenario>  the i915 test build (I915_TESTS=y) running one scenario of
#                                                      src/drivers/gpu/i915/tests/execution/runner.c after the device
#                                                      start (e.g. ktest, eu, draw, r1, tex, t3, bl, lcdb, display_ktest);
#                                                      prints the test lines, then the usual lines of the vkdemo run;
#                                                      vkdemo starts TEST_WAIT_S (default 90) seconds later than
#                                                      usual, so a long scenario ends before the application runs
#        plan/ws031/tests/vkloop-hw.sh "test <scenario> -DFOO=1"             (flags after the scenario)
#        CAPTURE=zdesktop plan/ws031/tests/vkloop-hw.sh zdesktop
#                                                      WS035 p066: zdesktop (zwl --glass at 1920x1080, two wl_shm windows and mview --windowed
#                                                      on top; services in plan/ws031/tests/zdesktop/)
#                                                      built with plan/ws031/tests/config-zdesktop-hw.mk into build/resident-zdesktop;
#                                                      the font and the wallpaper come from build/ws035-fonts/ and build/ws035-wallpaper/
#                                                      (not in git); the capture harness docks mview and opens Wiseview
#        Every run takes the machine: flock /tmp/i915-hw.lock plan/ws031/tests/vkloop-hw.sh ...
#        I915_HOST names the 5330 for ssh and scp (default: the alias solaris10-man; e.g. I915_HOST=awe@10.0.30.3)
set -u
cd "$(dirname "$0")/../../.."
EXTRA=${1:-}
TIME_MS=${2:-}
ORACLE=0
DISPLAY_RUN=0
SCENARIO=
case "$EXTRA" in test*)
	set -- ${EXTRA#test} $TIME_MS
	SCENARIO=${1:-}
	[ -n "$SCENARIO" ] || { echo "usage: $0 test <scenario>"; exit 2; }
	shift
	I915_TESTS=y
	EXTRA="-DI915_TEST_SCENARIO=$SCENARIO $*"
	TIME_MS= ;;
esac
WAYLAND_RUN=0
case "$EXTRA" in wayland*)
	WAYLAND_RUN=1
	EXTRA="${EXTRA#wayland}" ;;
esac
ZDESKTOP_RUN=0
case "$EXTRA" in zdesktop*)
	ZDESKTOP_RUN=1
	EXTRA="${EXTRA#zdesktop}"
	export ZEDBSD_CONFIG=${ZEDBSD_CONFIG:-plan/ws031/tests/config-zdesktop-hw.mk}
	BUILD=${BUILD:-build/resident-zdesktop} ;;
esac
I915_HOST=${I915_HOST:-solaris10-man}
MVIEW_RUN=0
case "$EXTRA" in mview*)
	MVIEW_RUN=1
	EXTRA="${EXTRA#mview}" ;;
esac
# a capture run is the capture build (p014 A0)
[ -z "${CAPTURE:-}" ] || I915_TEST_CAPTURE=y
case "$EXTRA" in display*)
	DISPLAY_RUN=1
	EXTRA="${EXTRA#display}"
	TIME_MS=${TIME_MS:-2500} ;;
esac
case "$EXTRA" in oracle*)
	ORACLE=1
	I915_TESTS=y
	I915_TEST_ORACLE=y
	EXTRA="${EXTRA#oracle}" ;;
esac
# XXX: the run is the QEMU passthrough, whose guest sees no OpRegion, so the kernel always carries the
# test machine's captured VBT (I915_TEST_VBT=y, vendor/intel-vbt/).  Drop it once the GPU tests run on bare metal.
I915_TEST_VBT=y
# I915_TEST_CAPTURE=y (from the environment, default n): the capture display in place of the panel -- the LCD is not
# brought up and every presentation is copied into guest RAM for the host's pmemsave (display/capture.c)
I915_TEST_CAPTURE=${I915_TEST_CAPTURE:-n}
BUILD=${BUILD:-build/resident}
mkdir -p $BUILD
# a change of flags or of the test build is not seen by make: force the rebuild and relink by hand
FLAGS="$EXTRA|${I915_TESTS:-n}|${I915_TEST_ORACLE:-n}|vbt=$I915_TEST_VBT|capture=$I915_TEST_CAPTURE"
[ -f $BUILD/.vkloop-flags ] && [ "$(cat $BUILD/.vkloop-flags)" = "$FLAGS" ] || {
	touch src/drivers/gpu/i915/i915.c
	# the scenario is read by the runner only
	[ ! -f src/drivers/gpu/i915/tests/execution/runner.c ] || touch src/drivers/gpu/i915/tests/execution/runner.c
	# the test VBT is read by these only
	touch src/drivers/gpu/i915/display/vbt.c src/drivers/gpu/i915/display/display.c
	[ ! -f src/drivers/gpu/i915/tests/execution/ktest-display.c ] || touch src/drivers/gpu/i915/tests/execution/ktest-display.c
	# the capture display is read by these only (display.c is touched above)
	touch src/drivers/gpu/i915/display/capture.c
}
# the probe service as this run wants it; the file changes (and the cached image is rebuilt) only when its text does
PROBE=$BUILD/vkprobe1.gen
if [ "$DISPLAY_RUN" = 1 ] && [ "$TIME_MS" = live ]; then
	sed "s/--offscreen --readback --token=vk1 --duration=1/--token=vk1 --duration=${LIVE_S:-12}/" plan/ws031/tests/vkprobe1 > $PROBE.new
elif [ "$DISPLAY_RUN" = 1 ]; then
	sed "s/--offscreen --readback --token=vk1 --duration=1/--readback --token=vk1 --time-ms=$TIME_MS --hold=20/" plan/ws031/tests/vkprobe1 > $PROBE.new
elif [ -n "$TIME_MS" ]; then
	sed "s/--duration=1/--time-ms=$TIME_MS/" plan/ws031/tests/vkprobe1 > $PROBE.new
else
	cp plan/ws031/tests/vkprobe1 $PROBE.new
fi
cmp -s $PROBE.new $PROBE 2>/dev/null || mv $PROBE.new $PROBE
# the first wait as this run wants it: a test scenario runs before the node is served, so the application waits longer
WAIT1=$BUILD/vkwait1.gen
if [ -n "$SCENARIO" ]; then
	sed "s/^arguments=45\$/arguments=$((45 + ${TEST_WAIT_S:-90}))/" plan/ws031/tests/vkwait1 > $WAIT1.new
else
	cp plan/ws031/tests/vkwait1 $WAIT1.new
fi
cmp -s $WAIT1.new $WAIT1 2>/dev/null || mv $WAIT1.new $WAIT1
FILES="--file /etc/service.d/vkprobe1=$PROBE --file /etc/service.d/vkwait1=$WAIT1"
for n in vkwait2 poweroff; do
	FILES="$FILES --file /etc/service.d/$n=plan/ws031/tests/$n"
done
RC_CONF=plan/ws031/tests/vkprobe-rc.conf
if [ "$WAYLAND_RUN" = 1 ]; then
	# the compositor and its clients take the place of vkdemo: the display lease is exclusive
	FILES="--file /etc/service.d/vkwait1=$WAIT1 --file /etc/service.d/poweroff=plan/ws031/tests/poweroff"
	for n in zwl wlwait wltest1 wltest2 vkwait2; do
		FILES="$FILES --file /etc/service.d/$n=plan/ws031/tests/wayland/$n"
	done
	RC_CONF=plan/ws031/tests/wayland/rc.conf
fi
if [ "$MVIEW_RUN" = 1 ]; then
	# the compositor and the model viewer; the capture harness sends the input
	FILES="--file /etc/service.d/vkwait1=$WAIT1 --file /etc/service.d/poweroff=plan/ws031/tests/poweroff"
	# MVIEW_ARGS adds viewer options (e.g. MVIEW_ARGS=--spin=30 for the turning demonstration); the service
	# file changes (and the cached image is rebuilt) only when its text does
	# MVIEW_MODEL=test ships the test model (userland/base/mview/models/test/: opaque, cutout and blend materials)
	# to /usr/share/mview/test/ and shows it in place of qs40; the p013 Venus images are then no reference
	if [ "${MVIEW_MODEL:-}" = test ]; then
		MVIEW_ARGS="${MVIEW_ARGS:+$MVIEW_ARGS }--model=/usr/share/mview/test"
		for n in model.txt tex/0.pam tex/1.pam; do
			FILES="$FILES --file /usr/share/mview/test/$n=userland/base/mview/models/test/$n"
		done
	fi
	MVIEW1=$BUILD/mview1.gen
	sed "s|^arguments=\(.*\)$|arguments=\1${MVIEW_ARGS:+ $MVIEW_ARGS}|" plan/ws031/tests/mview/mview1 > $MVIEW1.new
	cmp -s $MVIEW1.new $MVIEW1 2>/dev/null || mv $MVIEW1.new $MVIEW1
	rm -f $MVIEW1.new
	FILES="$FILES --file /etc/service.d/mview1=$MVIEW1"
	for n in zwl wlwait vkwait2; do
		FILES="$FILES --file /etc/service.d/$n=plan/ws031/tests/mview/$n"
	done
	RC_CONF=plan/ws031/tests/mview/rc.conf
fi
if [ "$ZDESKTOP_RUN" = 1 ]; then
	# the compositor in Wiseman Mode and three clients; the font and the wallpaper are not in git
	FILES="--file /etc/service.d/vkwait1=$WAIT1"
	for n in zwl wlwait wlshm1 mwait wlshm2 mwait2 wlkill mview1 vkwait2 poweroff; do
		FILES="$FILES --file /etc/service.d/$n=plan/ws031/tests/zdesktop/$n"
	done
	# the compositor's and the viewer's output and the kernel's messages go to /var/log, read afterwards
	# from the disk image with plan/ws031/tests/ufs-cat.py (not from the serial log)
	for n in run-zwl.sh run-wlkill.sh run-mview.sh run-poweroff.sh; do
		FILES="$FILES --file /etc/zdesktop/$n=plan/ws031/tests/zdesktop/$n"
	done
	# ZDESKTOP_APP=terminal runs zdesktop-terminal (WS035 p068) where the model viewer runs, with the same log
	if [ "${ZDESKTOP_APP:-mview}" = terminal ]; then
		FILES="$FILES --file /etc/zdesktop/run-mview.sh=plan/ws031/tests/zdesktop/run-terminal.sh"
		FILES="$FILES --file /usr/share/fonts/zdesktop-mono.ttf=build/ws035-fonts/JetBrainsMono-Regular.ttf"
	fi
	# ZDESKTOP_APP=home (WS035 p069): nothing is started in the viewer's place; App Home starts the
	# applications (CAPTURE=zdesktop-home clicks them), and the logs are written out every two seconds
	if [ "${ZDESKTOP_APP:-mview}" = home ]; then
		FILES="$FILES --file /etc/zdesktop/run-mview.sh=plan/ws031/tests/zdesktop/run-home.sh"
		FILES="$FILES --file /usr/share/fonts/zdesktop-mono.ttf=build/ws035-fonts/JetBrainsMono-Regular.ttf"
	fi
	FILES="$FILES --file /usr/share/fonts/zdesktop.ttf=build/ws035-fonts/Inter.ttf"
	FILES="$FILES --file /usr/share/zdesktop/wallpaper.ppm=build/ws035-wallpaper/wallpaper-1080.ppm"
	RC_CONF=plan/ws031/tests/zdesktop/rc.conf
fi
# the image is rebuilt only when an input is newer than it: switching to an older rc.conf does not
# count, so the chosen one is copied to one path whose file changes only when its text does
RC_GEN=$BUILD/rc-conf.gen
cp "$RC_CONF" $RC_GEN.new
cmp -s $RC_GEN.new $RC_GEN 2>/dev/null || mv $RC_GEN.new $RC_GEN
rm -f $RC_GEN.new
RC_CONF=$RC_GEN
make -j"$(nproc)" BUILD=$BUILD "I915_TESTS=${I915_TESTS:-n}" "I915_TEST_ORACLE=${I915_TEST_ORACLE:-n}" \
	"I915_TEST_VBT=$I915_TEST_VBT" "I915_TEST_CAPTURE=$I915_TEST_CAPTURE" \
	"ZEDBSD_TEST_CPPFLAGS=-DGPU_IOCTL_TRACE=1 $EXTRA" \
	ZEDBSD_TEST_RC_CONF=$RC_CONF "ZEDBSD_TEST_EXTRA_FILES=$FILES" \
	ZEDBSD_TEST_IMAGE_TAG=vkprobe disk-image > /tmp/resident-build.log 2>&1 || {
	echo "BUILD FAILED (the image on the 5330 is NOT this tree):"
	grep -E ' error: |Error [0-9]' /tmp/resident-build.log | head
	exit 1
}
printf '%s' "$FLAGS" > $BUILD/.vkloop-flags
# the iGPU goes back to vfio-pci if a Venus run left it on the host i915 driver (bigbang/igpu-mode.sh)
ssh $I915_HOST bigbang/igpu-mode.sh vfio >/dev/null || { echo "iGPU is not on vfio-pci"; exit 1; }
scp -q $BUILD/hdd-image.img $I915_HOST:bigbang/guest-parity.img || exit 1
if [ -n "${CAPTURE:-}" ]; then
	# QEMU with a QMP socket and USB input; the harness starts once the old serial log is gone
	scp -q plan/ws031/tests/i915-capture.py $I915_HOST:bigbang/i915-capture.py || exit 1
	REF=
	# the per-pixel shading draws what the p013 Venus images do not show: the comparison is skipped
	case "${MVIEW_ARGS:-}" in *--shading=pixel*) MVIEW_NO_VENUS=1 ;; esac
	if [ "${MVIEW_NO_VENUS:-0}" = 1 ]; then
		REF=--no-venus
	elif [ "$CAPTURE" = mview ] && [ -z "${MVIEW_MODEL:-}" ] && [ -d plan/ws031/temp/remote/p013-mview-009/evidence ]; then
		ssh $I915_HOST 'mkdir -p bigbang/mview-venus-ref'
		scp -q plan/ws031/temp/remote/p013-mview-009/evidence/*.ppm $I915_HOST:bigbang/mview-venus-ref/
		REF=--reference=/home/awe/bigbang/mview-venus-ref
	fi
	ssh $I915_HOST 'cd ~/bigbang && rm -f run-parity-serial.log && QMP=1 ./run-parity-vk.sh >/dev/null 2>&1; cp run-parity-serial.log vkloop-last.log' &
	LAUNCHER=$!
	sleep 5
	ssh $I915_HOST "sudo -n rm -rf bigbang/capture-out; sudo -n python3 bigbang/i915-capture.py $CAPTURE --output=/home/awe/bigbang/capture-out $REF; sudo -n chown -R awe: bigbang/capture-out" > /tmp/capture-harness.out 2>&1
	wait $LAUNCHER
	rm -rf /tmp/capture-last
	scp -qr $I915_HOST:bigbang/capture-out /tmp/capture-last
	echo "--- capture $CAPTURE"
	cat /tmp/capture-harness.out
else
	ssh $I915_HOST 'cd ~/bigbang && rm -f run-parity-serial.log && ./run-parity-vk.sh >/dev/null 2>&1; cp run-parity-serial.log vkloop-last.log'
fi
scp -q $I915_HOST:bigbang/vkloop-last.log /tmp/vkloop-last.log
if [ "$ZDESKTOP_RUN" = 1 ]; then
	# the guest's own logs, from its disk
	scp -q plan/ws031/tests/ufs-cat.py tools/build/check-ufs-image.py $I915_HOST:bigbang/ || exit 1
	ssh $I915_HOST 'python3 bigbang/ufs-cat.py bigbang/guest-parity.img /var/log/zwl.log /var/log/wlkill.log /var/log/mview.log /var/log/dmesg.log /var/log/xzed.log /var/log/vk.log' > /tmp/zdesktop-guest-logs.txt 2>&1
	echo "--- guest logs: /tmp/zdesktop-guest-logs.txt ($(wc -l < /tmp/zdesktop-guest-logs.txt) lines)"
	# the viewer's own last word: closed by the capture's click on the bar's close button
	grep -E '^(MVIEW|ZTERM) (DONE|FAILED)' /tmp/zdesktop-guest-logs.txt || echo "the application did not end (no DONE or FAILED line)"
fi
if [ -n "$SCENARIO" ]; then
	echo "--- test $SCENARIO"
	# the runner's line, the suite tally and skips, and every verdict line a scenario logs
	grep -aE 'i915: (test |ktest|MCR-PROBE summary)|i915: .*(verdict|[A-Z0-9-]+ (PASS|FAIL|HANG|ERROR)[:( ]|[A-Z0-9-]+ cleanup|[A-Z0-9-]+ release:)' /tmp/vkloop-last.log | cut -c1-300 | head -100
	echo "--- vkdemo"
fi
grep -anE 'i915: vk|i915: capture: (base|lease)|gpu: ioctl|VKDEMO|vkdemo:|ZWL|WLTEST|wltest:|zwl:|MVIEW (START|DONE|FAILED)|mview:|resident|panic|fault|init: ' /tmp/vkloop-last.log | grep -v 'parity N0\|parity P\|expected_fault' | cut -c1-200 | head -60
if [ "$ORACLE" = 1 ]; then
	python3 plan/ws031/handover/tools/vkdump_verify.py /tmp/vkloop-last.log plan/ws014/tests /tmp/vkframe1.ppm "${TIME_MS:-0}"
fi
