# ws045-p008: config/config.x86_64.mk with the base userland only: no
# external packages (clang, openssh, openssl, libcxx, emacs), no firmware
# downloads and no desktop programs, so that an image with the WS045
# utilities builds in the worktree without the package caches, for the boot
# test.
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
ZEDBSD_VARIANT := native

override CONFIG_KERNEL_TEST_CHECKPOINTS := n
override CONFIG_BUF_CACHE_KIB := 0
override CONFIG_GPU_JOB_RESERVATION_MS := 10000
override CONFIG_GPU_JOB_EXECUTION_MS := 60000
override CONFIG_GPU_JOB_STOP_MS := 10000
override CONFIG_GPU_CONTROL_MS := 10000
override CONFIG_DRIVER_NE2000 := y
override CONFIG_DRIVER_PCI_UHCI := y
override CONFIG_DRIVER_PCI_EHCI := y
override CONFIG_DRIVER_PCI_XHCI := y
override CONFIG_DRIVER_PCI_NVME := y
override CONFIG_DRIVER_PCI_INTEL_AX211 := y
override CONFIG_DRIVER_PCI_VENUS := y
override CONFIG_DRIVER_PCI_I915 := y
override CONFIG_DRIVER_USB_STORAGE := y
override CONFIG_DRIVER_USB_CDC_NCM := y
override CONFIG_DRIVER_USB_CDC_ECM := y
override CONFIG_DRIVER_USB_HID := y
override CONFIG_DRIVER_USB_RTL8822BU := y
override CONFIG_DRIVER_GRAPHICS := y
override CONFIG_DRIVER_LGY98 := n

override CONFIG_USERLAND_PROGRAMS := admin ar at awk basename batch bc blkid cal cat cflow chgrp chmod chown cksum cmp comm compress cp crontab csplit curses cut cxref date dd delta df dhcpc diff dirname diskpart dmesg du ed expand expr fetch file find fmt fold fuser gencat get getconf gettext grep halt head host hostname iconv id ifconfig infocmp ipcrm ipcs join kill less link ln locale localedef logger login logname lp lpr ls m4 mailx mesg mkdir mkfifo mkfs mkswap more msgfmt mv newgrp ngettext nice nl nm nohup nslookup ntpdate od paste patch pathchk pax ping poweroff pr prs ps readlink realpath reboot renice rm rmdel rmdir route sact sccs sed service shutdown sleep sort split stat strings stty swapoff swapon sync tabs tail talk tee terminfo-extra terminfo tic time timeout top touch tput tr truncate tsort tty uname uncompress unexpand unget uniq unlink uudecode uuencode val wc what lspci lsusb which who whoami wifi write xargs zcat echo false printf test true seq tac egrep fgrep env make
