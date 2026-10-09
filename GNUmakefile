# Native FreeBSD goals avoid parsing the unrelated zedBSD cross toolchain rules.
KEILAND_FREEBSD_ENTRY_GOALS := keiland-freebsd keiland-freebsd-install
ifneq ($(strip $(MAKECMDGOALS)),)
ifeq ($(filter-out $(KEILAND_FREEBSD_ENTRY_GOALS),$(MAKECMDGOALS)),)
.PHONY: keiland-freebsd keiland-freebsd-install
# WS194: the packages first (asked before they are installed), then the build, then the install offered.
KEILAND_ASK ?= y
keiland-freebsd:
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh check freebsd
	$(MAKE) -f userland/desktop/keiland-freebsd.mk all
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh offer-install freebsd $(MAKE) -f userland/desktop/keiland-freebsd.mk install $(MAKEOVERRIDES)
keiland-freebsd-install:
	$(MAKE) -f userland/desktop/keiland-freebsd.mk install
else
include Makefile
endif
else
include Makefile
endif
