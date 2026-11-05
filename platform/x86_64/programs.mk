USER_CPPFLAGS := \
	$(CPPFLAGS) \
	-Iinclude \
	-Isrc \
	-I. \
	-DHAL_ARCH_AMD64 \
	-DKERN_USER_ABI_LP64

USER_CFLAGS := \
	$(CFLAGS)\
	-m64 \
	-march=x86-64 \
	-fno-pic \
	-fno-pie \
	-fno-stack-protector \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-fno-builtin \
	-fno-common \
	-ffunction-sections \
	-fdata-sections \
	-Os \
	-Wall \
	-Wextra \
	-Werror
