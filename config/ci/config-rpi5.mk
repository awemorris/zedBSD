# The Raspberry Pi 5 (BCM2712) image: the Pi 4's arm64 userland on the Pi 5's
# board support.  The console is the firmware's framebuffer (and the debug
# UART with CONFIG_RPI5_SERIAL_CONSOLE=y); the root is the SD card's UFS
# partition.  The USB ports behind the RP1 south bridge take keyboards,
# hubs and disks, its Ethernet port is en0, and the onboard Wi-Fi is wlan0
# with the bcm43455-firmware package.  The onboard Bluetooth's driver is
# off (CONFIG_DRIVER_RPI5_BT).  The fan connector follows the processor's
# temperature (CONFIG_DRIVER_RPI5_FAN).  An NVMe drive on an M.2 HAT
# behind the external PCIe port (config.txt's dtparam=pciex1) is a disk
# (CONFIG_DRIVER_PCI_NVME).  The GPU is not bound yet, so the
# login is the text console's, and the Pi 4's chip drivers are off (the
# Pi 5's are part of its board).
ZEDBSD_MENU_VERSION := 3
ZEDBSD_PLATFORM := rpi5
ZEDBSD_ARCHITECTURE := arm64
ZEDBSD_BOARD := rpi5
ZEDBSD_VARIANT := default

CONFIG_KERNEL_TEST_CHECKPOINTS := n
CONFIG_PCAT_SERIAL_MIRROR := n
CONFIG_INPUT_TEST_INJECT := n
CONFIG_SECURITY_KEY_TEST_LOOPBACK := n
CONFIG_BT_TEST_LOOPBACK := n
CONFIG_BUF_CACHE_KIB := 0
CONFIG_GPU_JOB_RESERVATION_MS := 10000
CONFIG_GPU_JOB_EXECUTION_MS := 60000
CONFIG_GPU_JOB_STOP_MS := 10000
CONFIG_GPU_CONTROL_MS := 10000
ZEDBSD_ROOTFS_DEVELOPMENT := y
ZEDBSD_GRAPHICAL_BOOT := n
ZEDBSD_BOOT_KERNEL_MESSAGES := y
ZEDBSD_GRAPHICAL_LOGIN := n
ZEDBSD_NOCT_ACCEL := n
CONFIG_DRIVER_NE2000 := n
CONFIG_DRIVER_PCI_UHCI := n
CONFIG_DRIVER_PCI_EHCI := n
CONFIG_DRIVER_PCI_XHCI := y
CONFIG_DRIVER_PCI_NVME := y
CONFIG_DRIVER_PCI_AHCI := n
CONFIG_DRIVER_PCI_INTEL_AX211 := n
CONFIG_DRIVER_PCI_VENUS := n
CONFIG_DRIVER_PCI_I915 := n
CONFIG_DRIVER_PCI_NVRTX := n
CONFIG_DRIVER_PCI_AMDRDNA := n
CONFIG_DRIVER_PCI_HDA := n
CONFIG_DRIVER_PCI_LPSS_I2C := n
CONFIG_DRIVER_USB_STORAGE := y
CONFIG_DRIVER_USB_CDC_NCM := n
CONFIG_DRIVER_USB_CDC_ECM := n
CONFIG_DRIVER_USB_HID := y
CONFIG_DRIVER_USB_HUB := y
CONFIG_DRIVER_USB_CCID := n
CONFIG_DRIVER_USB_BT := n
CONFIG_DRIVER_USB_RTL8822BU := n
CONFIG_DRIVER_GRAPHICS := y
CONFIG_DRIVER_ACPI := n
CONFIG_DRIVER_TYPEC := n
CONFIG_DRIVER_BCM2711_GENET := n
CONFIG_DRIVER_BCM2711_WIFI := n
CONFIG_DRIVER_BCM2711_BT := n
CONFIG_DRIVER_BCM2711_AUDIO := n
CONFIG_DRIVER_BCM2711_GPU := n
CONFIG_DRIVER_BCM2711_VCHIQ := n
CONFIG_DRIVER_BCM2711_VIDEO := n
CONFIG_DRIVER_LGY98 := n
CONFIG_INPUT_BRIDGE := n
CONFIG_RPI5_SERIAL_CONSOLE := n
CONFIG_DRIVER_RPI5_BT := n
CONFIG_DRIVER_RPI5_FAN := y

ZEDBSD_USER_PROGRAMS := account-admin admin ar at audiod awk base64 basename batch bc blkid bluetoothd bootlog bt cal cat cflow chgrp chmod chown cksum cmp comm compress cp crontab csplit curses cut cxref date dd delta df dhcpc diff dirname diskpart dmesg du echo ed egrep emacs env expand expr false fetch fgrep fidoctl file find fmt fold fuser gencat get getconf gettext grep halt head host hostname iconv id ifconfig infocmp install ipcrm ipcs join kill less libz-compat link ln locale localedef logger login logname lp lpr ls lspci lsusb m4 mailx make mesg mkdir mkfifo mkfs mkswap mktemp more msgfmt mv netclock newgrp ngettext nice nl nm noct nohup nslookup ntpdate od passkey-fido2 passkey passwd paste patch pathchk pax ping poweroff pr printf prs ps pwd readlink realpath reboot renice rm rmdel rmdir route sact sccs sed seq service shutdown sleep sort split stat strings stty su sudo swapoff swapon sync tabs tac tail talk tee terminfo-extra terminfo test tic time timeout top touch tput tr true truncate tsort tty uname uncompress unexpand unget uniq unlink useradd userdel usermod uudecode uuencode val vi volumed wc what which who whoami wifi write xargs zcat browser calendar files imageview keiland-ime ime-dict-ja ime-dict-skk libgl libbrowser libegl libgif-compat libglesv2 libjpeg-compat libkeiland libmedia libpdf libpng-compat libtruetype libvulkan libwayland-egl libwayland-client mailer mediastorage monitor music notes pdfviewer phone photos keiland-preview keiland-printd sessiond settings terminal textedit videoplayer wayland xserver bcm43455-firmware brcmbt-firmware hand-hershey noto-color-emoji zlib curl openssh ca-certificates openssl
