# Test-only getty lifetime probe; never installed in ordinary images.
WS002_LIFECYCLE_SOURCE := plan/ws002/tests/missing-login-lifecycle.c
WS002_LIFECYCLE_ELF := $(BUILD)/ws002-lifecycle.elf
WS002_LIFECYCLE_OBJECT := $(BUILD)/$(WS002_LIFECYCLE_SOURCE:.c=.o)
WS002_LIFECYCLE_LINKER := $(if $(filter pc98,$(ZEDBSD_PLATFORM_DIR)),\
	$(PC98)/noct-user.ld,$(PCAT)/user.ld)

$(WS002_LIFECYCLE_OBJECT): $(WS002_LIFECYCLE_SOURCE)
	@mkdir -p $(dir $@)
	$(CC) $(ZEDBSD_CPPFLAGS) $(USER_CFLAGS) -MMD -MP -c $< -o $@

$(WS002_LIFECYCLE_ELF): $(USER_LIBC_OBJS) $(WS002_LIFECYCLE_OBJECT) \
	$(ZEDBSD_SOFTFLOAT_OBJECTS) $(WS002_LIFECYCLE_LINKER) $(USER_ELF_CHECK)
	$(LD) -m elf_i386 --gc-sections -nostdlib -static \
		-z max-page-size=4096 $(USER_STACK_LDFLAGS) \
		-T $(WS002_LIFECYCLE_LINKER) $(USER_LIBC_OBJS) \
		$(WS002_LIFECYCLE_OBJECT) $(ZEDBSD_SOFTFLOAT_OBJECTS) -o $@
	@test -z "$$(nm -u $@)" || { nm -u $@; exit 1; }
	$(NOCT) --path=$(BUILD_TOOLS_DIR) $(USER_ELF_CHECK) $@
.PHONY: ws002-lifecycle-fixture
ws002-lifecycle-fixture: $(WS002_LIFECYCLE_ELF)
