#include <stdio.h>
#include <stdlib.h>
#include "../src/hal/arm64/bsp-rpi4/fdt.h"

void *hal_memset(void *p, int c, size_t n)
{ unsigned char *d = p; while (n-- != 0) *d++ = (unsigned char)c; return p; }

int main(int argc, char **argv)
{
	FILE *file;
	long length;
	unsigned char *data;
	struct rpi4_fdt_info info;
	int error;
	if (argc != 2) return 2;
	file = fopen(argv[1], "rb");
	if (!file || fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) <= 0 ||
	    fseek(file, 0, SEEK_SET) != 0) return 2;
	data = malloc((size_t)length);
	if (!data || fread(data, 1, (size_t)length, file) != (size_t)length) return 2;
	fclose(file);
	error = rpi4_fdt_parse(data, (size_t)length, &info);
	free(data);
	if (error != 0) { fprintf(stderr, "FDT error: %s\n", rpi4_fdt_error(error)); return 1; }
	if (!info.compatible_rpi4 || info.uart_base != 0xfe201000ULL ||
	    info.mailbox_base != 0xfe00b880ULL || info.gic_dist_base != 0xff841000ULL ||
	    info.gic_cpu_base != 0xff842000ULL || info.sdhci_base != 0xfe340000ULL ||
	    info.sdhci_irq != 158) {
		fprintf(stderr, "unexpected devices: uart=%llx mbox=%llx gic=%llx/%llx sd=%llx irq=%u\n",
		    (unsigned long long)info.uart_base, (unsigned long long)info.mailbox_base,
		    (unsigned long long)info.gic_dist_base, (unsigned long long)info.gic_cpu_base,
		    (unsigned long long)info.sdhci_base, info.sdhci_irq);
		return 1;
	}
	puts("Raspberry Pi 4 FDT host test: PASS");
	return 0;
}
