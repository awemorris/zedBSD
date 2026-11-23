# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# Writes <APK>.sha256, in the form sha256sum prints, for the apk named with -DAPK=<file>.
#

file(SHA256 ${APK} sum)
get_filename_component(name ${APK} NAME)
file(WRITE ${APK}.sha256 "${sum}  ${name}\n")
