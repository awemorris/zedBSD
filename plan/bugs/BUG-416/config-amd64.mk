# The guest image of the USB network adapter's zero-length packet test (qemu-usbnet.sh): a small amd64 guest on the
# serial console (no SSH, no desktop) with the network (networkd with DHCP on the USB adapter ue0, net.conf of
# plan/tools/guest), ping and fetch, so that the guest reaches the host as 10.0.2.2.
include config/ci/config-amd64.mk
CONFIG_PCAT_SERIAL_MIRROR := y
ZEDBSD_GRAPHICAL_BOOT := n
ZEDBSD_GRAPHICAL_LOGIN := n
ZEDBSD_USER_PROGRAMS := networkd dhcpc ifconfig ping fetch cat echo ls uname sync true false sleep seq dmesg printf timeout
