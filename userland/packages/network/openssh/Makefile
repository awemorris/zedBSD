# zedBSD userland package: OpenSSH portable (external release tarball).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
#
# An external package, never part of the system: it is built and put in an
# image only when the configuration names it, and it needs OpenSSL, which the
# configuration names as well.
# The system's own ssh, sshd, ssh-keygen and sftp-server are userland/base/ssh
# (on libcrypto-compat).
#
# OpenSSH is BSD/ISC licensed and stays outside the source tree: only this
# Makefile, the patches beside it and the recorded archive identity live here.
OPENSSH_MAKEFILE := $(lastword $(MAKEFILE_LIST))
OPENSSH_DIR := $(patsubst %/,%,$(dir $(OPENSSH_MAKEFILE)))
include $(OPENSSH_DIR)/../../../base/package.mk
include $(OPENSSH_DIR)/../../external.mk

# The latest portable release when this was recorded. Upstream publishes a PGP
# signature rather than a digest file; the recorded digest is what three
# independent mirrors served. The signature is kept beside the archive for a
# separate out-of-band check.
EXT_openssh_VERSION := 10.5p1
EXT_openssh_ROOT := openssh-10.5p1
EXT_openssh_ARCHIVE := openssh-10.5p1.tar.gz
EXT_openssh_URL := https://cdn.openbsd.org/pub/OpenBSD/OpenSSH/portable/openssh-10.5p1.tar.gz
EXT_openssh_SIZE := 2333659
EXT_openssh_SHA256 := d44d28a839ea9daf969cc69150fde59910b2b39361dad81a3bd6cbd19218db11
EXT_openssh_PATCH_LEVEL := zedbsd0
EXT_openssh_PATCHES := $(sort $(wildcard $(abspath $(OPENSSH_DIR))/patches/*.patch))

$(eval $(call EXTERNAL_SOURCE,openssh))

# ---------------------------------------------------------------- build

# OpenSSH drives its own configure and Makefile, so the repository build waits
# on the staged files rather than on anything inside the external tree.
OPENSSH_CONFIGURED := $(EXT_openssh_BUILDDIR)/.zedbsd-configured
OPENSSH_STAGED := $(EXT_openssh_STAGEDIR)/.zedbsd-staged
OPENSSH_SSH := $(EXT_openssh_STAGEDIR)/usr/bin/ssh
OPENSSH_SSHD := $(EXT_openssh_STAGEDIR)/usr/sbin/sshd
OPENSSH_KEYGEN := $(EXT_openssh_STAGEDIR)/usr/bin/ssh-keygen
OPENSSH_SCP := $(EXT_openssh_STAGEDIR)/usr/bin/scp
OPENSSH_SFTP_SERVER := \
	$(EXT_openssh_STAGEDIR)/usr/libexec/sftp-server
OPENSSH_SESSION := \
	$(EXT_openssh_STAGEDIR)/usr/libexec/sshd-session
OPENSSH_AUTH := \
	$(EXT_openssh_STAGEDIR)/usr/libexec/sshd-auth
OPENSSH_FILES_DIR := $(abspath $(OPENSSH_DIR))/files
OPENSSH_SSHD_CONFIG := $(OPENSSH_FILES_DIR)/sshd_config
OPENSSH_SSH_CONFIG := $(OPENSSH_FILES_DIR)/ssh_config

# The definition init reads to start the server.  It is installed with
# the package, so a system built without OpenSSH has no definition and
# the entry rc.conf carries for it is passed over as optional.
OPENSSH_SERVICE := $(OPENSSH_FILES_DIR)/sshd.service

# The server is started through this rather than directly, so that a
# machine makes its own host key the first time it runs one.
OPENSSH_START := $(OPENSSH_FILES_DIR)/sshd-start
OPENSSH_LICENSE := $(EXT_openssh_SRCDIR)/LICENCE

# --without-zlib: nothing here provides it, and the protocol works without
#   compression.
# --disable-strip: the staged binaries are checked before they are installed.
# The crypto comes from the staged OpenSSL, which is built for the same
# target and is not installed anywhere a compiler would look by itself.
# The staged OpenSSL is named by its path rather than by the variable the
# OpenSSL makefile sets, because this file is read first and the variable
# would still be empty: an empty prerequisite is no ordering at all, and the
# configure would then run while the other package was rebuilding.
OPENSSH_SSL_STAGE := $(EXTERNAL_WORKROOT)/openssl/stage
OPENSSH_SSL_STAMP := $(OPENSSH_SSL_STAGE)/.zedbsd-staged

$(OPENSSH_CONFIGURED): $(EXT_openssh_SRCSTAMP) \
	$(EXTERNAL_CROSS_STAMP) $(OPENSSH_SSL_STAMP)
	@rm -rf '$(EXT_openssh_BUILDDIR)'
	@mkdir -p '$(EXT_openssh_BUILDDIR)'
	cd '$(EXT_openssh_BUILDDIR)' && \
		. '$(EXTERNAL_CROSS_ENV)' && \
		'$(EXT_openssh_SRCDIR)/configure' \
			--host="$$CROSS_TRIPLE" \
			--prefix=/usr --sysconfdir=/etc/ssh \
			--with-privsep-path=/var/empty \
			--with-privsep-user=sshd \
			--with-ssl-dir='$(OPENSSH_SSL_STAGE)/usr' \
			--without-zlib --without-pam --disable-strip \
			CC="$$CC" AR="$$AR" RANLIB="$$RANLIB" \
			LDFLAGS="-L$(OPENSSH_SSL_STAGE)/usr/lib -Wl,-rpath-link,$(OPENSSH_SSL_STAGE)/usr/lib"
	@touch '$@'

$(OPENSSH_STAGED): $(OPENSSH_CONFIGURED)
	@rm -rf '$(EXT_openssh_STAGEDIR)'
	$(MAKE) -C '$(EXT_openssh_BUILDDIR)'
	$(MAKE) -C '$(EXT_openssh_BUILDDIR)' \
		DESTDIR='$(EXT_openssh_STAGEDIR)' install-nokeys
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed libutil.so --needed libcrypto.so --needed libc.so \
		'$(OPENSSH_SSHD)'
	@$(PYTHON) '$(EXTERNAL_ROOT)/tools/build/check-dynamic-elf.py' \
		--machine $(ARCHITECTURE) --role application \
		--needed libutil.so --needed libcrypto.so --needed libc.so \
		'$(OPENSSH_SSH)'
	@touch '$@'

$(OPENSSH_SSH) $(OPENSSH_SSHD) $(OPENSSH_KEYGEN) \
$(OPENSSH_SCP) $(OPENSSH_SFTP_SERVER) \
$(OPENSSH_SESSION) $(OPENSSH_AUTH): $(OPENSSH_STAGED)
	@test -f '$@'

# The notice is taken from the extracted tree rather than the staged one.
$(OPENSSH_LICENSE): $(EXT_openssh_SRCSTAMP)
	@test -f '$@'

.PHONY: openssh
openssh: $(OPENSSH_STAGED)
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
PACKAGE_INPUTS += $(if $(filter openssh,$(USER_PROGRAMS)),\
	$(OPENSSH_SSH) $(OPENSSH_SSHD) $(OPENSSH_KEYGEN) \
	$(OPENSSH_SCP) $(OPENSSH_SFTP_SERVER) \
	$(OPENSSH_SESSION) $(OPENSSH_AUTH) \
	$(OPENSSH_SSHD_CONFIG) $(OPENSSH_SSH_CONFIG) \
	$(OPENSSH_SERVICE) $(OPENSSH_START) \
	$(OPENSSH_LICENSE))
PACKAGE_FILES += $(if $(filter openssh,$(USER_PROGRAMS)),\
	--file $(PACKAGE_BINDIR)/ssh=$(OPENSSH_SSH) \
	--file /usr/sbin/sshd=$(OPENSSH_SSHD) \
	--file $(PACKAGE_BINDIR)/ssh-keygen=$(OPENSSH_KEYGEN) \
	--file $(PACKAGE_BINDIR)/scp=$(OPENSSH_SCP) \
	--file /usr/libexec/sftp-server=$(OPENSSH_SFTP_SERVER) \
	--file /usr/libexec/sshd-session=$(OPENSSH_SESSION) \
	--file /usr/libexec/sshd-auth=$(OPENSSH_AUTH) \
	--file /etc/ssh/sshd_config=$(OPENSSH_SSHD_CONFIG) \
	--file /etc/ssh/ssh_config=$(OPENSSH_SSH_CONFIG) \
	--file /etc/service.d/sshd=$(OPENSSH_SERVICE) \
	--file /usr/libexec/sshd-start=$(OPENSSH_START) \
	--file /usr/share/licenses/openssh/LICENCE=$(OPENSSH_LICENSE) \
	--mode /usr/sbin/sshd=0755 --mode /etc/ssh/sshd_config=0600 \
	--mode /usr/libexec/sshd-start=0755)
