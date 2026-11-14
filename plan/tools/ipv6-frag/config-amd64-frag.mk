# The SSH guest (plan/tools/guest/config-amd64-ssh.mk) with ifconfig, ping, sysctl and udp6-recv, for
# plan/tools/ipv6-frag/qemu-frag.sh: IPv6 fragments from this host reassembled on the guest.
# Build: plan/tools/guest/test-image.sh plan/tools/ipv6-frag/config-amd64-frag.mk BUILD
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/plan/tools/guest/config-amd64-ssh.mk
override CONFIG_USERLAND_PROGRAMS += ifconfig ping sysctl udp6-recv
