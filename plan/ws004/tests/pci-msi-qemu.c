/* QEMU EDU MSI delivery fixture; built only with kernel test checkpoints. */
/* Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */

#include <drivers/pci.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define EDU_VENDOR 0x1234U
#define EDU_PRODUCT 0x11e8U
#define EDU_IRQ_STATUS 0x24U
#define EDU_IRQ_RAISE 0x60U
#define EDU_IRQ_ACK 0x64U

struct edu_fixture {
	struct drv_pci_device *device;
	struct drv_pci_mapping registers;
	struct drv_pci_irq irq;
	void *irq_cookie;
};

static struct edu_fixture fixture;

static void
allocator_handler(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	(void)irq;
	(void)acknowledge;
	(void)argument;
}

static int
allocator_probe(void)
{
	int irqs[16], extra = 0x1234;
	paddr_t address = (paddr_t)0x1234U;
	uint32_t event = 0x1234U;
	unsigned index;
	for (index = 0; index < 16U; index++)
		if (hal_irq_register_msi("PCI 0000:00:04.0", allocator_handler,
		    NULL, &irqs[index], &address, &event) != HAL_OK)
			goto fail;
	if (hal_irq_register_msi("PCI 0000:00:04.0", allocator_handler, NULL,
	    &extra, &address, &event) != HAL_ERR_NOMEM)
		goto fail;
	if (hal_irq_unregister_msi(irqs[5]) != HAL_OK ||
	    hal_irq_register_msi("PCI 0000:00:04.0", allocator_handler, NULL,
	    &extra, &address, &event) != HAL_OK || extra != irqs[5])
		goto fail;
	irqs[5] = extra;
	for (index = 0; index < 16U; index++)
		if (hal_irq_unregister_msi(irqs[index]) != HAL_OK)
			return EIO;
	hal_printf("WS004 MSI ALLOCATOR PASS vectors=16\n");
	return 0;
fail:
	while (index-- != 0)
		(void)hal_irq_unregister_msi(irqs[index]);
	return EIO;
}

static int
edu_irq(void *argument)
{
	struct edu_fixture *edu = argument;
	volatile uint32_t *registers = edu->registers.address;
	uint32_t status = registers[EDU_IRQ_STATUS / 4U];
	registers[EDU_IRQ_ACK / 4U] = status;
	hal_io_mb();
	hal_printf("WS004 MSI DELIVERY PASS status=%x\n", status);
	return 1;
}

static int
edu_attach(struct drv_pci_device *device, const struct drv_pci_id *id)
{
	unsigned count = 0;
	volatile uint32_t *registers;
	unsigned capability;
	uint16_t msi_control;
	uint16_t msi_data;
	uint32_t msi_address_low, msi_address_high;
	int error;
	(void)id;
	memset(&fixture, 0, sizeof(fixture));
	fixture.device = device;
	if ((error = drv_pci_device_claim_bar(device, 0)) != 0 ||
	    (error = drv_pci_device_map_bar(device, 0,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &fixture.registers)) != 0 ||
	    (error = drv_pci_device_enable_memory(device)) != 0 ||
	    (error = drv_pci_device_set_bus_master(device, true)) != 0 ||
	    (error = drv_pci_device_allocate_irqs(device, DRV_PCI_IRQ_ALLOW_MSI,
	    1, 1, &fixture.irq, &count)) != 0 || count != 1 ||
	    (error = drv_pci_device_establish_irq(device, &fixture.irq, edu_irq,
	    &fixture, "ws004-edu", &fixture.irq_cookie)) != 0)
		goto fail;
	registers = fixture.registers.address;
	if (drv_pci_device_find_capability(device, 0x05U, &capability) != 0 ||
	    drv_pci_device_config_read16(device, capability + 2U,
	    &msi_control) != 0 ||
	    drv_pci_device_config_read32(device, capability + 4U,
	    &msi_address_low) != 0 ||
	    drv_pci_device_config_read32(device, capability + 8U,
	    &msi_address_high) != 0 ||
	    drv_pci_device_config_read16(device, capability + 12U,
	    &msi_data) != 0) {
		error = EIO;
		goto fail;
	}
	hal_printf("WS004 MSI ARMED cap=%x control=%x addr=%x:%x data=%x id=%x\n",
	    capability, msi_control, msi_address_high, msi_address_low, msi_data,
	    registers[0]);
	drv_pci_device_set_driver_data(device, &fixture);
	return 0;
fail:
	if (fixture.irq_cookie != NULL)
		drv_pci_device_disestablish_irq(device, fixture.irq_cookie);
	if (count != 0)
		drv_pci_device_free_irqs(device, &fixture.irq, count);
	if (fixture.registers.address != NULL)
		drv_pci_device_unmap_bar(device, &fixture.registers);
	drv_pci_device_release_bar(device, 0);
	return error != 0 ? error : EIO;
}

static int
edu_detach(struct drv_pci_device *device, unsigned flags)
{
	(void)flags;
	if (fixture.irq_cookie != NULL)
		drv_pci_device_disestablish_irq(device, fixture.irq_cookie);
	drv_pci_device_free_irqs(device, &fixture.irq, 1);
	drv_pci_device_unmap_bar(device, &fixture.registers);
	drv_pci_device_release_bar(device, 0);
	memset(&fixture, 0, sizeof(fixture));
	return 0;
}

static const struct drv_pci_id edu_ids[] = {
	{ EDU_VENDOR, EDU_PRODUCT, DRV_PCI_ANY_ID, DRV_PCI_ANY_ID, 0, 0, 0 }
};

static struct drv_pci_driver edu_driver = {
	.name = "ws004-edu-msi",
	.ids = edu_ids,
	.id_count = sizeof(edu_ids) / sizeof(edu_ids[0]),
	.attach = edu_attach,
	.detach = edu_detach
};

int
ws004_pci_msi_qemu_register(void)
{
	if (allocator_probe() != 0)
		return EIO;
	return drv_pci_driver_register(&edu_driver);
}

void
ws004_pci_msi_qemu_raise(void)
{
	volatile uint32_t *registers = fixture.registers.address;
	if (registers == NULL || fixture.irq_cookie == NULL) {
		hal_printf("WS004 MSI RAISE SKIP\n");
		return;
	}
	registers[EDU_IRQ_RAISE / 4U] = 1U;
	hal_io_mb();
	hal_printf("WS004 MSI RAISED status=%x\n",
	    registers[EDU_IRQ_STATUS / 4U]);
}
