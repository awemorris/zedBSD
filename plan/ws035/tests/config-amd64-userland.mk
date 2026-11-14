# ws035-p001: config/config.x86_64.mk without firmware downloads and zedinst (userland baseline).
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk
ZEDBSD_VARIANT := native
# ws035-p098: the test images (this one and every config built on it: the lean Venus images, the guest images,
# the i915 hardware images) keep the console: the kernel's messages on it and the console login, so a test that
# starts zdesktop itself finds the display free.  The graphical boot is tested with config-amd64-graphical.mk.
override CONFIG_BOOT_SPLASH := n

override CONFIG_KERNEL_TEST_CHECKPOINTS := n
override CONFIG_BUF_CACHE_KIB := 0
override CONFIG_DRIVER_NE2000 := n
override CONFIG_DRIVER_PCI_UHCI := y
override CONFIG_DRIVER_PCI_EHCI := y
override CONFIG_DRIVER_PCI_XHCI := y
override CONFIG_DRIVER_PCI_NVME := y
override CONFIG_DRIVER_PCI_INTEL_AX211 := y
override CONFIG_DRIVER_USB_STORAGE := y
override CONFIG_DRIVER_USB_CDC_NCM := y
override CONFIG_DRIVER_USB_CDC_ECM := y
override CONFIG_DRIVER_USB_HID := y
override CONFIG_DRIVER_USB_RTL8822BU := y
override CONFIG_DRIVER_GRAPHICS := y
override CONFIG_DRIVER_LGY98 := n

override CONFIG_USERLAND_PROGRAMS := lspci lsusb libkeiland zshell zterm zwm admin ar at awk basename batch bc blkid cal cat cflow chgrp chmod chown cksum cmp comm compress cp crontab csplit curses cut cxref date dd delta df dhcpc diff dirname diskpart dmesg du ed expand expr fetch file find fmt fold fuser gencat get getconf gettext grep halt head host hostname iconv id ifconfig infocmp ipcrm ipcs join kill less link ln locale localedef logger login logname lp lpr ls m4 mailx mesg mkdir mkfifo mkfs mkswap more msgfmt mv newgrp ngettext nice nl nm noct nohup nslookup ntpdate od paste patch pathchk pax ping poweroff pr prs ps readlink realpath reboot renice rm rmdel rmdir route sact sccs sed service shutdown sleep sort split stat strings stty swapoff swapon tabs tail talk tee terminfo-extra terminfo tic time timeout top touch tput tr truncate tsort tty uname uncompress unexpand unget uniq unlink uudecode uuencode val wc what who wifi write xargs zcat sync echo false printf test true seq tac whoami egrep fgrep env make which

# Exercise the public Noct installer in the maintained amd64 profile.

# ws129-p010 (2026-10-02 user: every desktop application in the CI and the test configurations, and every base
# program that is not a test): the base programs this list lacked, and the desktop's applications, daemons and
# libraries with App Home's X11 Gears.  Test clients stay where each test configuration adds them.
override CONFIG_USERLAND_PROGRAMS += audiod base64 install mktemp pwd \
	libgif-compat libjpeg-compat libpdf libpng-compat libz-compat \
	libvulkan libwayland libwayland-egl libegl libglesv2 libGL libtruetype libkeiland \
	wayland sessiond terminal files notes pdfviewer imageview textedit settings browser libbrowser xserver zgears \
	ime dict
