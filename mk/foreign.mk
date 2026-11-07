keiland-linux:
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh check linux
	$(MAKE) -f userland/desktop/keiland-linux.mk all
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh offer-install linux $(MAKE) -f userland/desktop/keiland-linux.mk install $(MAKEOVERRIDES)

keiland-linux-install:
	$(MAKE) -f userland/desktop/keiland-linux.mk install

keiland-freebsd:
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh check freebsd
	$(MAKE) -f userland/desktop/keiland-freebsd.mk all
	@KEILAND_ASK=$(KEILAND_ASK) sh tools/build/keiland-prerequisites.sh offer-install freebsd $(MAKE) -f userland/desktop/keiland-freebsd.mk install $(MAKEOVERRIDES)

keiland-freebsd-install:
	$(MAKE) -f userland/desktop/keiland-freebsd.mk install
