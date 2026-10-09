# ws126-p005: the beta 2 release's configuration with python3 (and, through its requirements, zlib, OpenSSL and the CA
# bundle), plus what the guest tests need beside the image's own python3: the test suite (test/) and the test-only
# extension modules (_test*), which the package leaves out of an image, from the package's stage of this same build,
# and the tests of plan/ws126/tests in /root/ws126.  A test image only.
#   plan/tools/guest/test-image.sh plan/ws126/tests/config-amd64-python3.mk build/ws126-image
# then plan/ws126/tests/python3-guest.sh against the started guest.
include config/release/config-amd64-beta2.mk
ZEDBSD_USER_PROGRAMS += $(filter-out $(ZEDBSD_USER_PROGRAMS),python3)
ZEDBSD_PYTHON3_TEST_STAGE := $(CURDIR)/build/packages/python3/stage/usr/lib/python3.14
ZEDBSD_EXTRA_INPUTS += $(CURDIR)/build/packages/python3/stage/.zedbsd-staged
ZEDBSD_EXTRA_FILES += --subtree /usr/lib/python3.14/test=$(ZEDBSD_PYTHON3_TEST_STAGE)/test \
	--subtree /usr/lib/python3.14/lib-dynload=$(ZEDBSD_PYTHON3_TEST_STAGE)/lib-dynload \
	--file /root/ws126/smoke.py=plan/ws126/tests/smoke.py \
	--file /root/ws126/tls-loopback.py=plan/ws126/tests/tls-loopback.py \
	--file /root/ws126/repl-pty.py=plan/ws126/tests/repl-pty.py
