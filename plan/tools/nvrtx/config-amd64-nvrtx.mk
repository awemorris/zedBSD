# ws037-p004: the image for the nvrtx runs on the RTX 2070 handed to a QEMU guest by VFIO
# (fb1f6afee: plan/ws037/phase004/phase.md section 3).  The CI amd64 image (config/config.x86_64.mk) with:
#   - the nvrtx driver built in (CONFIG_DRIVER_PCI_NVRTX, on by default since 2026-10-15),
#   - the GSP firmware package (nvidia-gsp-firmware; its license is accepted first with
#     "yes | make firmware-licenses", as for every image with a firmware package),
#   - the text console with the kernel's lines on it (no graphical boot or login), so the
#     stage marks are on the guest's emulated VGA screen that QMP's screendump captures,
#   - the boot words of one run.  Since 2026-10-15 the display stages run by default (the GSP boot, P4m,
#     P8a, the takeover P8b showing the console, the GPU device P8c), so each run turns off what it does
#     not test: NVRTX_RUN=R0 (nvrtx.gsp=0: identify and console only), R1 (nvrtx.gsp=1 nvrtx.stop=devinit), R2 (stop=booter),
#     R3 (stop=static-info), R4 (stop=address-space), R5 (stop=copy-channel), R6 (stop=channel-stop) or R7 (nvrtx.gsp=1, every built stage up to
#     P8a: nvrtx.disp=0 nvrtx.resident=0) or R7g (R7 and nvrtx.gr=1, the golden context) or R8b (the
#     takeover of the head the GOP lit as the monitor test, nvrtx.disp.test=1, and nvrtx.resident=0) or R8c
#     (R8b with the GPU device) or R8d (R8c with the display-scanout probe: events, refresh, cursor,
#     sharing) or R9w (the work channel, stage D1) or R9v (the renderer, stage D3, with vkquad) or R9k (the desktop, D6) or RAk/RAd (method A with nvrtx.vk=1, D7) or RA (no words: the default boot of the main image, the console kept and shown on the
#     taken-over head, the GPU device); fb1f6afee: plan/ws037/phase004 to phase006 give what each run must show.
# Build one image per run, each into its own BUILD:
#   plan/tools/guest/test-image.sh --no-harness plan/tools/nvrtx/config-amd64-nvrtx.mk build/ws037-nvrtx/r0 NVRTX_RUN=R0
# The tree, found from this file: the packages' nested makes read the configuration from their own
# directories, so the files it includes are named by absolute path.
TEST_TREE := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../../..)
include $(TEST_TREE)/config/config.x86_64.mk
include $(TEST_TREE)/mk/platform-x86_64/defaults.mk

override CONFIG_DRIVER_PCI_NVRTX := y
override CONFIG_USERLAND_PROGRAMS += nvidia-gsp-firmware

override CONFIG_BOOT_SPLASH := n
override CONFIG_GRAPHICAL_LOGIN := n
override CONFIG_QUIET_BOOT := n

NVRTX_RUN ?= R0
ifeq ($(NVRTX_RUN),R0)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=0
endif
# ws037-p006 method A: the main image's default boot (the console kept through the GSP boot and shown on the head).
ifeq ($(NVRTX_RUN),RA)
ZEDBSD_BOOT_EXTRA_LINES :=
override CONFIG_USERLAND_PROGRAMS += display-inventory display-control
endif
ifeq ($(NVRTX_RUN),R1)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=devinit
endif
ifeq ($(NVRTX_RUN),R2)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=booter
endif
ifeq ($(NVRTX_RUN),R3)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=static-info
endif
ifeq ($(NVRTX_RUN),R4)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=address-space
endif
ifeq ($(NVRTX_RUN),R5)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=copy-channel
endif
ifeq ($(NVRTX_RUN),R6)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.stop=channel-stop
endif
ifeq ($(NVRTX_RUN),R7)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp=0 nvrtx.resident=0
endif
# ws037-p005 5f: R7 with the graphics golden context and the TU10x scrubber (stage P5g).
ifeq ($(NVRTX_RUN),R7g)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.gr=1 nvrtx.disp=0 nvrtx.resident=0
endif
# ws037-p006 P8b: R7 with the takeover of the head the GOP lit (needs the RTX's option ROM run, rombar=1).
ifeq ($(NVRTX_RUN),R8b)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp.test=1 nvrtx.resident=0
endif
# ws037-p008 stage C: R7g with the first compute launch (stage P9c: a program the nvrtx compiler made writes 42).
ifeq ($(NVRTX_RUN),R7c)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.gr=1 nvrtx.compute=1 nvrtx.disp=0 nvrtx.resident=0
endif
# ws037-p007 7b/7c: R7c with the first draw on the compute channel (stage P9g: a quad into a system-memory and a VRAM
# target, then textured).
ifeq ($(NVRTX_RUN),R7d)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.gr=1 nvrtx.compute=1 nvrtx.draw=1 nvrtx.disp=0 nvrtx.resident=0
endif
# ws037-p006 P8c: R8b with the GSP kept up and the GPU registered as a GPU device (the default since 2026-10-15),
# and the display probes that drive it from the serial console (display-inventory, display-control).
ifeq ($(NVRTX_RUN),R8c)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp.test=1
override CONFIG_USERLAND_PROGRAMS += display-inventory display-control
endif
# ws037-p006 P8c (2): R8c's device with the display's events, refresh, cursor plane, blob sharing and scanout constraints,
# and the probe that drives the new parts from the serial console (display-scanout) besides R8c's two.
ifeq ($(NVRTX_RUN),R8d)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp.test=1
override CONFIG_USERLAND_PROGRAMS += display-inventory display-control display-scanout
endif
# ws037-p007 D1: R7 with the work channel the GPU device will keep (stage P9w: the golden context, the channel with
# the 3D and compute objects, its first fence), stopped again at the end.
ifeq ($(NVRTX_RUN),R9w)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.work=1 nvrtx.disp=0 nvrtx.resident=0
endif
# ws037-p007 D3/D5: R8c's GPU device (in the VFIO guest the firmware's console is on the emulated VGA, not the RTX, so the
# takeover needs nvrtx.disp.test=1's own surfaces; without it the head is left to the firmware and no device is
# registered, T1-672) as a renderer too (nvrtx.vk=1, with the work channel it implies); vkquad, the
# compositor's textured quad drawn offscreen through Vulkan and read back (one line: VKQUAD ok / VKQUAD fail ...),
# and display-events, a swapchain of the display cleared and presented through VK_KHR_display (the WSI of stage D5).
# ws037-p007 D7 (method A): the main image's default boot (graphical boot and login, no kernel lines on the screen) with the
# renderer (nvrtx.vk=1, off by default), so the console is kept through the GSP boot and the compositor starts on the RTX;
# RAd is RAk with the kernel lines on the screen and no boot logo, to see the last mark when RAk fails (phase007 8.14).
ifeq ($(NVRTX_RUN),RAk)
override CONFIG_BOOT_SPLASH := y
override CONFIG_GRAPHICAL_LOGIN := y
override CONFIG_QUIET_BOOT := y
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.vk=1
override CONFIG_USERLAND_PROGRAMS += display-inventory
endif
ifeq ($(NVRTX_RUN),RAd)
override CONFIG_BOOT_SPLASH := n
override CONFIG_GRAPHICAL_LOGIN := y
override CONFIG_QUIET_BOOT := n
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.vk=1
override CONFIG_USERLAND_PROGRAMS += display-inventory
endif
ifeq ($(NVRTX_RUN),R9v)
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp.test=1 nvrtx.vk=1
override CONFIG_USERLAND_PROGRAMS += vkquad display-events
endif
# ws037-p007 D6: the desktop on the RTX: R9v's renderer, the takeover with the driver's own surface at once
# (nvrtx.disp.own=1: the test's 16 s of waits would outlast sessiond's 15 s wait for the display, T1-676), with the
# graphical boot and login the main image has (sessiond
# starts the compositor on /dev/gpu0, the RTX), the compositor's capture channel and keiland-shot (what it composes, to
# a PNG fetched over SSH), and R9v's two probes.
ifeq ($(NVRTX_RUN),R9k)
override CONFIG_BOOT_SPLASH := y
override CONFIG_GRAPHICAL_LOGIN := y
override CONFIG_QUIET_BOOT := y
ZEDBSD_BOOT_EXTRA_LINES := nvrtx.gsp=1 nvrtx.disp.own=1 nvrtx.vk=1
ZEDBSD_TEST_SCREEN_CAPTURE := y
override CONFIG_USERLAND_PROGRAMS += vkquad display-events keiland-shot
endif
