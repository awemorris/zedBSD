# Native FreeBSD entry for the base system's BSD make, which reads BSDmakefile before Makefile.
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
# GNU make selects GNUmakefile and keeps the full project build interface.
GMAKE?= gmake
NATIVE_MAKE= env -u MAKEFLAGS -u MFLAGS ${GMAKE} -j ${.MAKE.JOBS:U1} -f userland/desktop/keiland-freebsd.mk
NATIVE_OVERRIDES= ${.MAKEOVERRIDES:@name@${name}=${${name}:Q}@}

# WS194: the packages first (gmake among them, asked before they are installed), then the build, then the install
# offered; KEILAND_ASK=n asks nothing and only prints.
KEILAND_ASK?= y
PREREQUISITES= KEILAND_ASK=${KEILAND_ASK} sh tools/build/keiland-prerequisites.sh

keiland-freebsd:
	@${PREREQUISITES} check freebsd
	${NATIVE_MAKE} all ${NATIVE_OVERRIDES}
	@${PREREQUISITES} offer-install freebsd ${NATIVE_MAKE} install ${NATIVE_OVERRIDES}

keiland-freebsd-install:
	${NATIVE_MAKE} install ${NATIVE_OVERRIDES}

help:
	@printf '%s\n' 'make keiland-freebsd: build with the native FreeBSD compiler' \
	    'sudo make keiland-freebsd-install: install under /opt/keiland' \
	    'Other project targets require GNU make (gmake).'

.PHONY: keiland-freebsd keiland-freebsd-install help
