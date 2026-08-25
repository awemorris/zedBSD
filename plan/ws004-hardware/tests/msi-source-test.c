#include "src/hal/amd64/irq.h"

#include <assert.h>

int
main(void)
{
	assert(amd64_msi_source_valid("PCI 0000:00:00.0"));
	assert(amd64_msi_source_valid("PCI ffff:ff:1f.7"));
	assert(!amd64_msi_source_valid(NULL));
	assert(!amd64_msi_source_valid(""));
	assert(!amd64_msi_source_valid("PCI 0000:00:00"));
	assert(!amd64_msi_source_valid("PCI 0000:00:20.0"));
	assert(!amd64_msi_source_valid("PCI 0000:00:00.8"));
	assert(!amd64_msi_source_valid("PCI 0000:00:0A.0"));
	assert(!amd64_msi_source_valid("PCI 0000:00:00.0 "));
	assert(!amd64_msi_source_valid("USB 0000:00:00.0"));
	return 0;
}
