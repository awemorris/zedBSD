# zedBSD userland package: OpenSSH portable (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# OpenSSH is BSD/ISC licensed and stays outside the source tree: only this
# Makefile, the patches beside it and the recorded archive identity live here.
# Provenance and the license audit are in plan/ws032/provenance.md.
#
# Registration as a selectable package and the cross build land with the build
# work; this file currently owns acquisition of the verified source tree.
ZEDBSD_OPENSSH_MAKEFILE := $(lastword $(MAKEFILE_LIST))
ZEDBSD_OPENSSH_DIR := $(patsubst %/,%,$(dir $(ZEDBSD_OPENSSH_MAKEFILE)))
include $(ZEDBSD_OPENSSH_DIR)/../../../base/package.mk
include $(ZEDBSD_OPENSSH_DIR)/../../external.mk

# The latest portable release when this was recorded. Upstream publishes a PGP
# signature rather than a digest file; the recorded digest is what three
# independent mirrors served. The signature is kept beside the archive for a
# separate out-of-band check.
ZEDBSD_EXT_openssh_VERSION := 10.5p1
ZEDBSD_EXT_openssh_ROOT := openssh-10.5p1
ZEDBSD_EXT_openssh_ARCHIVE := openssh-10.5p1.tar.gz
ZEDBSD_EXT_openssh_URL := https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.5p1.tar.gz
ZEDBSD_EXT_openssh_SIZE := 2333659
ZEDBSD_EXT_openssh_SHA256 := d44d28a839ea9daf969cc69150fde59910b2b39361dad81a3bd6cbd19218db11
ZEDBSD_EXT_openssh_PATCH_LEVEL := zedbsd0
ZEDBSD_EXT_openssh_PATCHES := $(sort $(wildcard $(abspath $(ZEDBSD_OPENSSH_DIR))/patches/*.patch))

$(eval $(call ZEDBSD_EXTERNAL_SOURCE,openssh))

# ---------------------------------------------------------------- build

# OpenSSH drives its own configure and Makefile, so the repository build waits
# on the staged files rather than on anything inside the external tree.
ZEDBSD_OPENSSH_CONFIGURED := $(ZEDBSD_EXT_openssh_BUILDDIR)/.zedbsd-configured
ZEDBSD_OPENSSH_STAGED := $(ZEDBSD_EXT_openssh_STAGEDIR)/.zedbsd-staged
ZEDBSD_OPENSSH_SSH := $(ZEDBSD_EXT_openssh_STAGEDIR)/usr/bin/ssh
ZEDBSD_OPENSSH_SSHD := $(ZEDBSD_EXT_openssh_STAGEDIR)/usr/sbin/sshd
ZEDBSD_OPENSSH_KEYGEN := $(ZEDBSD_EXT_openssh_STAGEDIR)/usr/bin/ssh-keygen
ZEDBSD_OPENSSH_SCP := $(ZEDBSD_EXT_openssh_STAGEDIR)/usr/bin/scp
ZEDBSD_OPENSSH_SFTP_SERVER := \
	$(ZEDBSD_EXT_openssh_STAGEDIR)/usr/libexec/sftp-server
ZEDBSD_OPENSSH_SESSION := \
	$(ZEDBSD_EXT_openssh_STAGEDIR)/usr/libexec/sshd-session
ZEDBSD_OPENSSH_AUTH := \
	$(ZEDBSD_EXT_openssh_STAGEDIR)/usr/libexec/sshd-auth
ZEDBSD_OPENSSH_FILES_DIR := $(abspath $(ZEDBSD_OPENSSH_DIR))/files
ZEDBSD_OPENSSH_SSHD_CONFIG := $(ZEDBSD_OPENSSH_FILES_DIR)/sshd_config
ZEDBSD_OPENSSH_SSH_CONFIG := $(ZEDBSD_OPENSSH_FILES_DIR)/ssh_config

# The definition init reads to start the server.  It is installed with
# the package, so a system built without OpenSSH has no definition and
# the entry rc.conf carries for it is passed over as optional.
ZEDBSD_OPENSSH_SERVICE := $(ZEDBSD_OPENSSH_FILES_DIR)/sshd.service

# The server is started through this rather than directly, so that a
# machine makes its own host key the first time it runs one.
ZEDBSD_OPENSSH_START := $(ZEDBSD_OPENSSH_FILES_DIR)/sshd-start
ZEDBSD_OPENSSH_LICENSE := $(ZEDBSD_EXT_openssh_SRCDIR)/LICENCE

# --without-zlib: nothing here provides it, and the protocol works without
#   compression.
# --disable-strip: the staged binaries are checked before they are installed.
# The crypto comes from the staged OpenSSL, which is built for the same
# target and is not installed anywhere a compiler would look by itself.
# The staged OpenSSL is named by its path rather than by the variable the
# OpenSSL makefile sets, because this file is read first and the variable
# would still be empty: an empty prerequisite is no ordering at all, and the
# configure would then run while the other package was rebuilding.
ZEDBSD_OPENSSH_SSL_STAGE := $(ZEDBSD_EXTERNAL_WORKROOT)/openssl/stage
ZEDBSD_OPENSSH_SSL_STAMP := $(ZEDBSD_OPENSSH_SSL_STAGE)/.zedbsd-staged

$(ZEDBSD_OPENSSH_CONFIGURED): $(ZEDBSD_EXT_openssh_SRCSTAMP) \
	$(ZEDBSD_EXTERNAL_CROSS_STAMP) $(ZEDBSD_OPENSSH_SSL_STAMP)
	@rm -rf '$(ZEDBSD_EXT_openssh_BUILDDIR)'
	@mkdir -p '$(ZEDBSD_EXT_openssh_BUILDDIR)'
	cd '$(ZEDBSD_EXT_openssh_BUILDDIR)' && \
		. '$(ZEDBSD_EXTERNAL_CROSS_ENV)' && \
		'$(ZEDBSD_EXT_openssh_SRCDIR)/configure' \
			--host="$$ZEDBSD_CROSS_TRIPLE" \
			--prefix=/usr --sysconfdir=/etc/ssh \
			--with-privsep-path=/var/empty \
			--with-privsep-user=sshd \
			--with-ssl-dir='$(ZEDBSD_OPENSSH_SSL_STAGE)/usr' \
			--without-zlib --without-pam --disable-strip \
			CC="$$CC" AR="$$AR" RANLIB="$$RANLIB" \
			LDFLAGS="-L$(ZEDBSD_OPENSSH_SSL_STAGE)/usr/lib -Wl,-rpath-link,$(ZEDBSD_OPENSSH_SSL_STAGE)/usr/lib"
	@touch '$@'

$(ZEDBSD_OPENSSH_STAGED): $(ZEDBSD_OPENSSH_CONFIGURED)
	@rm -rf '$(ZEDBSD_EXT_openssh_STAGEDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_openssh_BUILDDIR)'
	$(MAKE) -C '$(ZEDBSD_EXT_openssh_BUILDDIR)' \
		DESTDIR='$(ZEDBSD_EXT_openssh_STAGEDIR)' install-nokeys
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed libutil.so --needed libcrypto.so --needed libc.so \
		'$(ZEDBSD_OPENSSH_SSHD)'
	@$(PYTHON) '$(ZEDBSD_EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ZEDBSD_ARCHITECTURE) --role application \
		--needed libutil.so --needed libcrypto.so --needed libc.so \
		'$(ZEDBSD_OPENSSH_SSH)'
	@touch '$@'

$(ZEDBSD_OPENSSH_SSH) $(ZEDBSD_OPENSSH_SSHD) $(ZEDBSD_OPENSSH_KEYGEN) \
$(ZEDBSD_OPENSSH_SCP) $(ZEDBSD_OPENSSH_SFTP_SERVER) \
$(ZEDBSD_OPENSSH_SESSION) $(ZEDBSD_OPENSSH_AUTH): $(ZEDBSD_OPENSSH_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one.
$(ZEDBSD_OPENSSH_LICENSE): $(ZEDBSD_EXT_openssh_SRCSTAMP)
	@test -f '$@'

.PHONY: openssh
openssh: $(ZEDBSD_OPENSSH_STAGED)
	@:

# ---------------------------------------------------------------- packaging

# The server and the client are built against OpenSSL, so choosing OpenSSH
# chooses OpenSSL as well: the requirement is named here rather than left to
# whoever configures the build to remember.
$(eval $(call USERLAND_PACKAGE,openssh,OpenSSH secure shell,*,n,\
	package,,y,packages/network,network/openssh,security/openssl))

# /var/empty is the directory the unprivileged half of the server moves its
# root to before it reads anything from the network.  The image builder makes
# a directory by placing a file in it, and the file placed here is the one
# that says why the directory exists.
ZEDBSD_PACKAGE_INPUTS += $(if $(filter openssh,$(ZEDBSD_USER_PROGRAMS)),\
	$(ZEDBSD_OPENSSH_SSH) $(ZEDBSD_OPENSSH_SSHD) $(ZEDBSD_OPENSSH_KEYGEN) \
	$(ZEDBSD_OPENSSH_SCP) $(ZEDBSD_OPENSSH_SFTP_SERVER) \
	$(ZEDBSD_OPENSSH_SESSION) $(ZEDBSD_OPENSSH_AUTH) \
	$(ZEDBSD_OPENSSH_SSHD_CONFIG) $(ZEDBSD_OPENSSH_SSH_CONFIG) \
	$(ZEDBSD_OPENSSH_SERVICE) $(ZEDBSD_OPENSSH_START) \
	$(ZEDBSD_OPENSSH_LICENSE))
ZEDBSD_PACKAGE_FILES += $(if $(filter openssh,$(ZEDBSD_USER_PROGRAMS)),\
	--file $(ZEDBSD_PACKAGE_BINDIR)/ssh=$(ZEDBSD_OPENSSH_SSH) \
	--file /usr/sbin/sshd=$(ZEDBSD_OPENSSH_SSHD) \
	--file $(ZEDBSD_PACKAGE_BINDIR)/ssh-keygen=$(ZEDBSD_OPENSSH_KEYGEN) \
	--file $(ZEDBSD_PACKAGE_BINDIR)/scp=$(ZEDBSD_OPENSSH_SCP) \
	--file /usr/libexec/sftp-server=$(ZEDBSD_OPENSSH_SFTP_SERVER) \
	--file /usr/libexec/sshd-session=$(ZEDBSD_OPENSSH_SESSION) \
	--file /usr/libexec/sshd-auth=$(ZEDBSD_OPENSSH_AUTH) \
	--file /etc/ssh/sshd_config=$(ZEDBSD_OPENSSH_SSHD_CONFIG) \
	--file /etc/ssh/ssh_config=$(ZEDBSD_OPENSSH_SSH_CONFIG) \
	--file /etc/service.d/sshd=$(ZEDBSD_OPENSSH_SERVICE) \
	--file /usr/libexec/sshd-start=$(ZEDBSD_OPENSSH_START) \
	--file /usr/share/licenses/openssh/LICENCE=$(ZEDBSD_OPENSSH_LICENSE) \
	--mode /usr/sbin/sshd=0755 --mode /etc/ssh/sshd_config=0600 \
	--mode /usr/libexec/sshd-start=0755)
