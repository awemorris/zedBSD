# The SSH guest (plan/tools/guest/config-amd64-ssh.mk) with sysctl, ifconfig and tcp-perf, for
# plan/ws221/tests/qemu-tcp.sh: TCP transfers both ways between the guest and this host.
# Build: plan/tools/guest/test-image.sh plan/ws221/tests/config-amd64-tcp.mk BUILD
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += sysctl ifconfig tcp-perf
