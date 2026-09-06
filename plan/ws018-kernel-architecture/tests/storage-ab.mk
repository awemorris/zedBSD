# Experimental settings apply only to the two translation units. A final
# ordinary build must force both objects to restore production defaults.
$(BUILD)/kern64/src/kern/syscall.o: AMD64_CPPFLAGS += -DZEDBSD_SYSCALL_REGULAR_CHUNK=$(Q086_CHUNK)
$(BUILD)/kern64/src/drivers/pci-xhci.o: AMD64_CPPFLAGS += -DZEDBSD_XHCI_IMOD=$(Q086_IMOD)
.PHONY: q086-ab-force
$(BUILD)/kern64/src/kern/syscall.o $(BUILD)/kern64/src/drivers/pci-xhci.o: q086-ab-force
