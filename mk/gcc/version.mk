# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

# The GNU toolchain's sources: the release tarballs from the GNU servers,
# each checked against its SHA-256 (the hashes were taken from tarballs whose
# GPG signatures matched the GNU keyring).

override ZEDBSD_BINUTILS_VERSION := 2.46.1
override ZEDBSD_BINUTILS_ARCHIVE_NAME := binutils-2.46.1.tar.xz
override ZEDBSD_BINUTILS_ARCHIVE_URL := https://ftp.gnu.org/gnu/binutils/binutils-2.46.1.tar.xz
override ZEDBSD_BINUTILS_ARCHIVE_SHA256 := e127a709cba24c76de8936cb7083dd768f28cd37eb010492e2f19b71eb1294e4

override ZEDBSD_GCC_VERSION := 14.4.0
override ZEDBSD_GCC_ARCHIVE_NAME := gcc-14.4.0.tar.xz
override ZEDBSD_GCC_ARCHIVE_URL := https://ftp.gnu.org/gnu/gcc/gcc-14.4.0/gcc-14.4.0.tar.xz
override ZEDBSD_GCC_ARCHIVE_SHA256 := 752b6f567beac83159c77a7680b1316bdd784738bff9a9d070112c09da90f6d9

# The libraries GCC itself needs, built with it from its source tree.
override ZEDBSD_GMP_ARCHIVE_NAME := gmp-6.3.0.tar.xz
override ZEDBSD_GMP_ARCHIVE_URL := https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz
override ZEDBSD_GMP_ARCHIVE_SHA256 := a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898

override ZEDBSD_MPFR_ARCHIVE_NAME := mpfr-4.2.2.tar.xz
override ZEDBSD_MPFR_ARCHIVE_URL := https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.2.tar.xz
override ZEDBSD_MPFR_ARCHIVE_SHA256 := b67ba0383ef7e8a8563734e2e889ef5ec3c3b898a01d00fa0a6869ad81c6ce01

override ZEDBSD_MPC_ARCHIVE_NAME := mpc-1.3.1.tar.gz
override ZEDBSD_MPC_ARCHIVE_URL := https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz
override ZEDBSD_MPC_ARCHIVE_SHA256 := ab642492f5cf882b74aa0cb730cd410a81edcdbec895183ce930e706c1c759b8
