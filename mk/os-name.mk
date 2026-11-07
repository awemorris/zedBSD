# The operating system's name for the build (/etc/os-release), read
# from include/kern/os-name.h so that the name is defined once:
# ZEDBSD_OS_NAME (os-release's NAME) and ZEDBSD_OS_ID (its ID).

OS_NAME := $(shell sed -n 's/^\#define ZEDBSDOS_NAME "\(.*\)"$$/\1/p' include/kern/os-name.h)
OS_ID := $(shell sed -n 's/^\#define ZEDBSD_OS_ID "\(.*\)"$$/\1/p' include/kern/os-name.h)

ifeq ($(strip $(OS_NAME)),)
$(error mk/os-name.mk: no ZEDBSD_OS_NAME in include/kern/os-name.h)
endif

ifeq ($(strip $(OS_ID)),)
$(error mk/os-name.mk: no ZEDBSD_OS_ID in include/kern/os-name.h)
endif
