/* Generic PCI bus core. Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib */
#include <drivers/pci.h>
#include <errno.h>
#include <hal/hal.h>
#include <string.h>

#define PCI_COMMAND 0x04U
#define PCI_STATUS 0x06U
#define PCI_CLASS_REVISION 0x08U
#define PCI_HEADER_TYPE 0x0eU
#define PCI_BAR0 0x10U
#define PCI_CAPABILITIES 0x34U
#define PCI_INTERRUPT_LINE 0x3cU
#define PCI_COMMAND_IO 0x0001U
#define PCI_COMMAND_MEMORY 0x0002U
#define PCI_COMMAND_MASTER 0x0004U
#define PCI_COMMAND_INTX_DISABLE 0x0400U
#define PCI_MSI_CONTROL 0x02U
#define PCI_MSI_ADDRESS 0x04U
#define PCI_MSI_ENABLE 0x0001U
#define PCI_MSI_64BIT 0x0080U
#define PCI_MSIX_CONTROL 0x02U
#define PCI_MSIX_TABLE 0x04U
#define PCI_MSIX_ENABLE 0x8000U
#define PCI_MSIX_FUNCTION_MASK 0x4000U
#define PCI_MSIX_ENTRY_SIZE 16U
#define PCI_MSIX_ENTRY_MASK 0x00000001U

struct drv_pci_bus {
	uint16_t segment;
	uint8_t number;
	const struct drv_pci_bus_ops *ops;
	void *host;
	struct drv_dma_device *dma;
	struct drv_pci_bus *parent, *next;
	struct drv_pci_device *bridge, *devices;
};

struct drv_pci_device {
	struct drv_pci_address address;
	struct drv_pci_bus *bus;
	struct drv_pci_device *next;
	struct drv_pci_bus *subordinate;
	struct drv_pci_driver *driver;
	void *driver_data;
	uint16_t vendor, product, subvendor, subproduct;
	uint32_t class_code;
	uint8_t revision, header_type;
	unsigned enable_count, bar_count;
	struct drv_pci_bar bars[6];
	uint8_t bar_claimed[6];
	uint32_t irq_claimed;
};

struct pci_driver_entry {
	struct drv_pci_driver *driver;
	struct pci_driver_entry *next;
};

struct pci_irq_cookie {
	int irq;
	drv_pci_irq_handler_t handler;
	void *argument;
	struct drv_pci_device *device;
	enum drv_pci_irq_type type;
	unsigned capability, index;
	struct drv_pci_mapping table;
	unsigned table_mapped;
};

static struct drv_pci_bus *root_buses;
static struct pci_driver_entry *drivers;
static int initialized;

static int cfg_read(struct drv_pci_bus *bus, const struct drv_pci_address *a,
	unsigned offset, unsigned width, uint32_t *value)
{
	unsigned limit = bus != NULL && bus->ops != NULL &&
		bus->ops->config_space_size != 0 ? bus->ops->config_space_size : 256U;
	if (bus == NULL || bus->ops == NULL || bus->ops->config_read == NULL ||
	    value == NULL || offset > limit || width > limit - offset ||
	    (width != 1 && width != 2 && width != 4))
		return EINVAL;
	return bus->ops->config_read(bus->host, a, offset, width, value);
}

static int cfg_write(struct drv_pci_bus *bus, const struct drv_pci_address *a,
	unsigned offset, unsigned width, uint32_t value)
{
	unsigned limit = bus != NULL && bus->ops != NULL &&
		bus->ops->config_space_size != 0 ? bus->ops->config_space_size : 256U;
	if (bus == NULL || bus->ops == NULL || bus->ops->config_write == NULL ||
	    offset > limit || width > limit - offset ||
	    (width != 1 && width != 2 && width != 4))
		return EINVAL;
	return bus->ops->config_write(bus->host, a, offset, width, value);
}

static int read_device(struct drv_pci_device *device)
{
	uint32_t value;
	uint32_t command = 0;
	unsigned index, limit;
	int error;
	error = cfg_read(device->bus, &device->address, 0, 4, &value);
	if (error != 0) return error;
	device->vendor = (uint16_t)value;
	device->product = (uint16_t)(value >> 16);
	(void)cfg_read(device->bus, &device->address, PCI_CLASS_REVISION, 4, &value);
	device->revision = (uint8_t)value;
	device->class_code = value >> 8;
	(void)cfg_read(device->bus, &device->address, 0x0cU, 4, &value);
	device->header_type = (uint8_t)(value >> 16);
	limit = (device->header_type & 0x7fU) == 1U ? 2U : 6U;
	device->bar_count = limit;
	/* BAR sizing must never decode the temporary all-ones address. */
	(void)cfg_read(device->bus, &device->address, PCI_COMMAND, 2, &command);
	(void)cfg_write(device->bus, &device->address, PCI_COMMAND, 2,
	    command & ~(PCI_COMMAND_IO | PCI_COMMAND_MEMORY));
	for (index = 0; index < limit; index++) {
		uint32_t original, mask;
		struct drv_pci_bar *bar = &device->bars[index];
		unsigned offset = PCI_BAR0 + index * 4U;
		memset(bar, 0, sizeof(*bar));
		bar->index = index;
		if (cfg_read(device->bus, &device->address, offset, 4, &original) != 0 ||
		    original == 0xffffffffU)
			continue;
		(void)cfg_write(device->bus, &device->address, offset, 4, 0xffffffffU);
		(void)cfg_read(device->bus, &device->address, offset, 4, &mask);
		(void)cfg_write(device->bus, &device->address, offset, 4, original);
		if (original & 1U) {
			bar->type = DRV_PCI_BAR_IO;
			bar->bus_address = original & ~3U;
			bar->size = mask == 0 ? 0 : (uint32_t)(~(mask & ~3U) + 1U);
		} else {
			bar->prefetchable = (original & 8U) != 0;
			bar->type = ((original >> 1) & 3U) == 2U ?
			    DRV_PCI_BAR_MEMORY64 : DRV_PCI_BAR_MEMORY32;
			bar->bus_address = original & ~15U;
			bar->size = mask == 0 ? 0 : (uint32_t)(~(mask & ~15U) + 1U);
			if (bar->type == DRV_PCI_BAR_MEMORY64 && index + 1U < limit) {
				uint32_t high, high_mask;
				(void)cfg_read(device->bus, &device->address, offset + 4U, 4, &high);
				(void)cfg_write(device->bus, &device->address, offset + 4U, 4, 0xffffffffU);
				(void)cfg_read(device->bus, &device->address, offset + 4U, 4, &high_mask);
				(void)cfg_write(device->bus, &device->address, offset + 4U, 4, high);
				bar->bus_address |= (uint64_t)high << 32;
				bar->size = ~( ((uint64_t)high_mask << 32) |
				    (mask & ~15U)) + 1U;
				index++;
			}
		}
	}
	(void)cfg_write(device->bus, &device->address, PCI_COMMAND, 2, command);
	if ((device->header_type & 0x7fU) == 0U) {
		(void)cfg_read(device->bus, &device->address, 0x2cU, 4, &value);
		device->subvendor = (uint16_t)value;
		device->subproduct = (uint16_t)(value >> 16);
	}
	return 0;
}

int drv_pci_init(void)
{
	if (initialized) return EALREADY;
	root_buses = NULL; drivers = NULL; initialized = 1;
	return 0;
}

void drv_pci_shutdown(void)
{
	struct drv_pci_bus *bus;
	for (bus = root_buses; bus != NULL; bus = bus->next) {
		struct drv_pci_device *device;
		for (device = bus->devices; device != NULL; device = device->next)
			if (device->driver != NULL && device->driver->shutdown != NULL)
				device->driver->shutdown(device);
	}
}

int drv_pci_bus_create_root(uint16_t segment, uint8_t number,
	const struct drv_pci_bus_ops *ops, void *host, struct drv_dma_device *dma,
	struct drv_pci_bus **result)
{
	struct drv_pci_bus *bus;
	if (!initialized || ops == NULL || ops->config_read == NULL ||
	    ops->config_write == NULL || result == NULL)
		return EINVAL;
	bus = hal_malloc(sizeof(*bus));
	if (bus == NULL) return ENOMEM;
	memset(bus, 0, sizeof(*bus));
	bus->segment = segment; bus->number = number; bus->ops = ops;
	bus->host = host; bus->dma = dma; bus->next = root_buses;
	root_buses = bus; *result = bus;
	return 0;
}

int drv_pci_bus_create_child(struct drv_pci_bus *parent,
	struct drv_pci_device *bridge, uint8_t number, struct drv_pci_bus **result)
{
	struct drv_pci_bus *bus;
	if (parent == NULL || bridge == NULL || result == NULL) return EINVAL;
	bus = hal_malloc(sizeof(*bus)); if (bus == NULL) return ENOMEM;
	memset(bus, 0, sizeof(*bus)); bus->segment = parent->segment;
	bus->number = number; bus->ops = parent->ops; bus->host = parent->host;
	bus->dma = parent->dma; bus->parent = parent; bus->bridge = bridge;
	bridge->subordinate = bus; *result = bus; return 0;
}

int drv_pci_bus_destroy(struct drv_pci_bus *bus)
{
	struct drv_pci_device *device, *next;
	struct drv_pci_bus **link;
	if (bus == NULL || bus->devices != NULL) return EBUSY;
	for (device = bus->devices; device != NULL; device = next) {
		next = device->next; hal_free(device);
	}
	if (bus->parent == NULL)
		for (link = &root_buses; *link != NULL; link = &(*link)->next)
			if (*link == bus) { *link = bus->next; break; }
	if (bus->bridge != NULL) bus->bridge->subordinate = NULL;
	hal_free(bus); return 0;
}

int drv_pci_device_probe(struct drv_pci_device *device);

static struct drv_pci_device *find_device_on_bus(struct drv_pci_bus *bus,
	const struct drv_pci_address *address)
{
	struct drv_pci_device *device;

	for (device = bus->devices; device != NULL; device = device->next)
		if (device->address.segment == address->segment &&
		    device->address.bus == address->bus &&
		    device->address.device == address->device &&
		    device->address.function == address->function)
			return device;
	return NULL;
}

int drv_pci_bus_scan(struct drv_pci_bus *bus)
{
	unsigned slot, function;
	if (bus == NULL) return EINVAL;
	for (slot = 0; slot < 32U; slot++) {
		unsigned functions = 1;
		for (function = 0; function < functions; function++) {
			struct drv_pci_address address = { bus->segment, bus->number,
			    (uint8_t)slot, (uint8_t)function };
			struct drv_pci_device *device;
			uint32_t value;
			if (cfg_read(bus, &address, 0, 4, &value) != 0 ||
			    (value & 0xffffU) == 0xffffU) continue;
			device = find_device_on_bus(bus, &address);
			if (device != NULL) {
				if (function == 0 && (device->header_type & 0x80U) != 0)
					functions = 8;
				continue;
			}
			device = hal_malloc(sizeof(*device)); if (device == NULL) return ENOMEM;
			memset(device, 0, sizeof(*device)); device->address = address;
			device->bus = bus;
			if (read_device(device) != 0) { hal_free(device); continue; }
			device->next = bus->devices; bus->devices = device;
			if (function == 0 && (device->header_type & 0x80U) != 0)
				functions = 8;
			if ((device->header_type & 0x7fU) == 1U) {
				uint8_t secondary = 0;
				if (drv_pci_device_config_read8(device, 0x19U,
				    &secondary) == 0 && secondary != 0 &&
				    secondary != bus->number)
					(void)drv_pci_bus_create_child(bus, device,
					    secondary, &device->subordinate);
			}
			(void)drv_pci_device_probe(device);
		}
	}
	return 0;
}

int drv_pci_bus_rescan(struct drv_pci_bus *bus) { return drv_pci_bus_scan(bus); }
int drv_pci_bus_scan_tree(struct drv_pci_bus *bus)
{struct drv_pci_device*d;int e;if((e=drv_pci_bus_scan(bus))!=0)return e;for(d=bus->devices;d;d=d->next)if(d->subordinate&&(e=drv_pci_bus_scan_tree(d->subordinate))!=0)return e;return 0;}
int drv_pci_scan_all(void)
{ struct drv_pci_bus *b; int e; for (b=root_buses;b;b=b->next) if ((e=drv_pci_bus_scan_tree(b))!=0) return e; return 0; }

int drv_pci_foreach_bus(drv_pci_bus_iterator_t fn, void *arg)
{ struct drv_pci_bus *b; int e; if(!fn)return EINVAL; for(b=root_buses;b;b=b->next)if((e=fn(b,arg))!=0)return e;return 0; }
static int foreach_device_tree(struct drv_pci_bus*b,drv_pci_device_iterator_t fn,void*arg)
{struct drv_pci_device*d;int e;for(d=b->devices;d;d=d->next){if((e=fn(d,arg))!=0)return e;if(d->subordinate&&(e=foreach_device_tree(d->subordinate,fn,arg))!=0)return e;}return 0;}
int drv_pci_foreach_device(drv_pci_device_iterator_t fn, void *arg)
{ struct drv_pci_bus*b;int e;if(!fn)return EINVAL;for(b=root_buses;b;b=b->next)if((e=foreach_device_tree(b,fn,arg))!=0)return e;return 0; }
uint16_t drv_pci_bus_segment(const struct drv_pci_bus*b){return b?b->segment:0;}
uint8_t drv_pci_bus_number(const struct drv_pci_bus*b){return b?b->number:0;}
struct drv_pci_bus *drv_pci_bus_parent(const struct drv_pci_bus*b){return b?b->parent:NULL;}
struct drv_pci_device *drv_pci_bus_bridge(const struct drv_pci_bus*b){return b?b->bridge:NULL;}
int drv_pci_bus_foreach_device(struct drv_pci_bus*b,drv_pci_device_iterator_t fn,void*arg)
{struct drv_pci_device*d;int e;if(!b||!fn)return EINVAL;for(d=b->devices;d;d=d->next)if((e=fn(d,arg))!=0)return e;return 0;}

struct drv_pci_device *drv_pci_find_device(const struct drv_pci_address*a)
{struct drv_pci_bus*b;struct drv_pci_device*d;if(!a)return NULL;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)if(memcmp(&d->address,a,sizeof(*a))==0)return d;return NULL;}
struct drv_pci_device *drv_pci_find_id(uint16_t v,uint16_t p,struct drv_pci_device*after)
{struct drv_pci_bus*b;struct drv_pci_device*d;int found=after==NULL;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next){if(!found){if(d==after)found=1;continue;}if(d->vendor==v&&d->product==p)return d;}return NULL;}
struct drv_pci_device *drv_pci_find_class(uint32_t c,uint32_t m,struct drv_pci_device*after)
{struct drv_pci_bus*b;struct drv_pci_device*d;int found=after==NULL;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next){if(!found){if(d==after)found=1;continue;}if((d->class_code&m)==(c&m))return d;}return NULL;}
struct drv_pci_bus*drv_pci_device_bus(const struct drv_pci_device*d){return d?d->bus:NULL;}
struct drv_pci_bus*drv_pci_device_subordinate_bus(const struct drv_pci_device*d){return d?d->subordinate:NULL;}
void drv_pci_device_address(const struct drv_pci_device*d,struct drv_pci_address*a){if(d&&a)*a=d->address;}
uint16_t drv_pci_device_vendor(const struct drv_pci_device*d){return d?d->vendor:DRV_PCI_ANY_ID;}
uint16_t drv_pci_device_product(const struct drv_pci_device*d){return d?d->product:DRV_PCI_ANY_ID;}
uint16_t drv_pci_device_subvendor(const struct drv_pci_device*d){return d?d->subvendor:DRV_PCI_ANY_ID;}
uint16_t drv_pci_device_subproduct(const struct drv_pci_device*d){return d?d->subproduct:DRV_PCI_ANY_ID;}
uint32_t drv_pci_device_class(const struct drv_pci_device*d){return d?d->class_code:0;}
uint8_t drv_pci_device_revision(const struct drv_pci_device*d){return d?d->revision:0;}
uint8_t drv_pci_device_header_type(const struct drv_pci_device*d){return d?d->header_type:0xff;}
bool drv_pci_device_is_bridge(const struct drv_pci_device*d){return d&&((d->class_code>>8)==0x0604U);}
bool drv_pci_device_is_multifunction(const struct drv_pci_device*d){return d&&(d->header_type&0x80U);}

int drv_pci_device_config_read8(struct drv_pci_device*d,unsigned o,uint8_t*v){uint32_t x;int e;if(!d||!v)return EINVAL;e=cfg_read(d->bus,&d->address,o,1,&x);*v=(uint8_t)x;return e;}
int drv_pci_device_config_read16(struct drv_pci_device*d,unsigned o,uint16_t*v){uint32_t x;int e;if(!d||!v)return EINVAL;e=cfg_read(d->bus,&d->address,o,2,&x);*v=(uint16_t)x;return e;}
int drv_pci_device_config_read32(struct drv_pci_device*d,unsigned o,uint32_t*v){return d?cfg_read(d->bus,&d->address,o,4,v):EINVAL;}
int drv_pci_device_config_write8(struct drv_pci_device*d,unsigned o,uint8_t v){return d?cfg_write(d->bus,&d->address,o,1,v):EINVAL;}
int drv_pci_device_config_write16(struct drv_pci_device*d,unsigned o,uint16_t v){return d?cfg_write(d->bus,&d->address,o,2,v):EINVAL;}
int drv_pci_device_config_write32(struct drv_pci_device*d,unsigned o,uint32_t v){return d?cfg_write(d->bus,&d->address,o,4,v):EINVAL;}

int drv_pci_device_find_capability(struct drv_pci_device*d,uint8_t id,unsigned*result)
{uint16_t status;uint8_t p,n;unsigned guard=0;if(!d||!result)return EINVAL;if(drv_pci_device_config_read16(d,PCI_STATUS,&status)||!(status&0x10U))return ENOENT;if(drv_pci_device_config_read8(d,PCI_CAPABILITIES,&p))return EIO;p&=~3U;while(p>=0x40U&&guard++<48U){if(drv_pci_device_config_read8(d,p,&n))return EIO;if(n==id){*result=p;return 0;}if(drv_pci_device_config_read8(d,p+1U,&p))return EIO;p&=~3U;}return ENOENT;}
int drv_pci_device_find_extended_capability(struct drv_pci_device*d,uint16_t id,unsigned start,unsigned*result)
{unsigned p=start?start:0x100U,guard=0,limit;uint32_t h;if(!d||!result||p<0x100U||(p&3U))return EINVAL;limit=d->bus->ops->config_space_size?d->bus->ops->config_space_size:256U;if(limit<4096U)return ENOTSUP;while(p>=0x100U&&p+4U<=limit&&guard++<960U){if(drv_pci_device_config_read32(d,p,&h)!=0)return EIO;if(h==0||h==0xffffffffU)return ENOENT;if((h&0xffffU)==id){*result=p;return 0;}if(((h>>20)&0xfffU)==0)return ENOENT;if(((h>>20)&0xfffU)<=p||(((h>>20)&0xfffU)&3U))return EIO;p=(h>>20)&0xfffU;}return EIO;}

static int command_set(struct drv_pci_device*d,uint16_t set,uint16_t clear)
{uint16_t v;int e;if(!d)return EINVAL;e=drv_pci_device_config_read16(d,PCI_COMMAND,&v);if(e)return e;return drv_pci_device_config_write16(d,PCI_COMMAND,(uint16_t)((v|set)&~clear));}
int drv_pci_device_enable(struct drv_pci_device*d){int e;if(!d)return EINVAL;if(d->enable_count++!=0)return 0;e=command_set(d,PCI_COMMAND_IO|PCI_COMMAND_MEMORY,0);if(e)d->enable_count--;return e;}
void drv_pci_device_disable(struct drv_pci_device*d){if(d&&d->enable_count&&--d->enable_count==0)(void)command_set(d,0,PCI_COMMAND_IO|PCI_COMMAND_MEMORY|PCI_COMMAND_MASTER);}
int drv_pci_device_enable_io(struct drv_pci_device*d){return command_set(d,PCI_COMMAND_IO,0);}
int drv_pci_device_enable_memory(struct drv_pci_device*d){return command_set(d,PCI_COMMAND_MEMORY,0);}
int drv_pci_device_set_bus_master(struct drv_pci_device*d,bool on){return command_set(d,on?PCI_COMMAND_MASTER:0,on?0:PCI_COMMAND_MASTER);}
unsigned drv_pci_device_bar_count(const struct drv_pci_device*d){return d?d->bar_count:0;}
int drv_pci_device_bar(const struct drv_pci_device*d,unsigned i,struct drv_pci_bar*b){if(!d||!b||i>=d->bar_count)return EINVAL;*b=d->bars[i];return b->type==DRV_PCI_BAR_NONE?ENOENT:0;}
int drv_pci_device_assign_bar(struct drv_pci_device*d,unsigned i,uint64_t a){struct drv_pci_bar*b;uint32_t low;uint16_t command;int e;if(!d||i>=d->bar_count)return EINVAL;b=&d->bars[i];if(b->type==DRV_PCI_BAR_NONE||b->size==0||(a&(b->size-1U))!=0)return EINVAL;if(b->type!=DRV_PCI_BAR_MEMORY64&&a>0xffffffffU)return EINVAL;e=drv_pci_device_config_read16(d,PCI_COMMAND,&command);if(e)return e;(void)drv_pci_device_config_write16(d,PCI_COMMAND,(uint16_t)(command&~(PCI_COMMAND_IO|PCI_COMMAND_MEMORY)));low=(uint32_t)a|(b->type==DRV_PCI_BAR_IO?1U:(b->prefetchable?8U:0U))|(b->type==DRV_PCI_BAR_MEMORY64?4U:0U);e=drv_pci_device_config_write32(d,PCI_BAR0+i*4U,low);if(!e&&b->type==DRV_PCI_BAR_MEMORY64)e=drv_pci_device_config_write32(d,PCI_BAR0+(i+1U)*4U,(uint32_t)(a>>32));(void)drv_pci_device_config_write16(d,PCI_COMMAND,command);if(!e)b->bus_address=a;return e;}
int drv_pci_device_claim_bar(struct drv_pci_device*d,unsigned i){if(!d||i>=d->bar_count)return EINVAL;if(d->bar_claimed[i])return EBUSY;d->bar_claimed[i]=1;return 0;}
void drv_pci_device_release_bar(struct drv_pci_device*d,unsigned i){if(d&&i<d->bar_count)d->bar_claimed[i]=0;}
int drv_pci_device_map_bar_region(struct drv_pci_device*d,unsigned i,uint64_t o,size_t s,unsigned f,struct drv_pci_mapping*m){struct drv_pci_bar b;if(!d||!m||i>=d->bar_count||!d->bar_claimed[i]||!d->bus->ops->map_bar||s==0)return EINVAL;b=d->bars[i];if(o>b.size||s>b.size-o)return EINVAL;b.bus_address+=o;b.size=s;return d->bus->ops->map_bar(d->bus->host,d,&b,f,m);}
int drv_pci_device_map_bar(struct drv_pci_device*d,unsigned i,unsigned f,struct drv_pci_mapping*m){if(!d||i>=d->bar_count)return EINVAL;return drv_pci_device_map_bar_region(d,i,0,(size_t)d->bars[i].size,f,m);}
void drv_pci_device_unmap_bar(struct drv_pci_device*d,struct drv_pci_mapping*m){if(d&&m&&d->bus->ops->unmap_bar)d->bus->ops->unmap_bar(d->bus->host,m);}

int
drv_pci_device_allocate_irqs(struct drv_pci_device *device, unsigned flags,
	unsigned minimum, unsigned maximum, struct drv_pci_irq *irqs,
	unsigned *count)
{
	static const struct {
		unsigned flag;
		enum drv_pci_irq_type type;
	} choices[] = {
		{ DRV_PCI_IRQ_ALLOW_MSIX, DRV_PCI_IRQ_MSIX },
		{ DRV_PCI_IRQ_ALLOW_MSI, DRV_PCI_IRQ_MSI },
		{ DRV_PCI_IRQ_ALLOW_INTX, DRV_PCI_IRQ_INTX }
	};
	unsigned choice;
	int error = ENOTSUP;
	if (device == NULL || irqs == NULL || count == NULL || minimum == 0 ||
	    maximum < minimum || device->bus->ops->allocate_irqs == NULL)
		return EINVAL;
	for (choice = 0; choice < sizeof(choices) / sizeof(choices[0]); choice++) {
		if ((flags & choices[choice].flag) == 0)
			continue;
		error = device->bus->ops->allocate_irqs(device->bus->host, device,
		    choices[choice].type, minimum, maximum, irqs, count);
		if (error == 0)
			return 0;
		if (error != ENOTSUP && error != ENODEV)
			return error;
	}
	return error;
}

void
drv_pci_device_free_irqs(struct drv_pci_device *device,
	struct drv_pci_irq *irqs, unsigned count)
{
	if (device != NULL && device->bus->ops->free_irqs != NULL)
		device->bus->ops->free_irqs(device->bus->host, device, irqs,
		    count);
}

static void
pci_irq_dispatch(int irq, hal_irq_ack_t acknowledge, void *argument)
{
	struct pci_irq_cookie *cookie = argument;
	(void)irq;
	(void)cookie->handler(cookie->argument);
	hal_irq_send_eoi(acknowledge);
}

static void
pci_source(const struct drv_pci_address *address, char result[17])
{
	static const char hex[] = "0123456789abcdef";
	result[0] = 'P'; result[1] = 'C'; result[2] = 'I'; result[3] = ' ';
	result[4] = hex[address->segment >> 12];
	result[5] = hex[(address->segment >> 8) & 15U];
	result[6] = hex[(address->segment >> 4) & 15U];
	result[7] = hex[address->segment & 15U]; result[8] = ':';
	result[9] = hex[address->bus >> 4];
	result[10] = hex[address->bus & 15U]; result[11] = ':';
	result[12] = hex[address->device >> 4];
	result[13] = hex[address->device & 15U]; result[14] = '.';
	result[15] = hex[address->function]; result[16] = '\0';
}

static int
establish_msi(struct pci_irq_cookie *cookie, const struct drv_pci_irq *irq)
{
	struct drv_pci_device *device = cookie->device;
	char source[17];
	paddr_t address;
	uint32_t event;
	uint16_t control;
	unsigned data_offset;
	int error;

	pci_source(&device->address, source);
	error = hal_irq_register_msi(source, pci_irq_dispatch, cookie,
	    &cookie->irq, &address, &event);
	if (error != HAL_OK)
		return error == HAL_ERR_NOMEM ? ENOMEM : EIO;
	if (event > UINT16_MAX ||
	    drv_pci_device_config_read16(device,
	    cookie->capability + PCI_MSI_CONTROL, &control) != 0) {
		error = EIO;
		goto fail;
	}
	control &= (uint16_t)~PCI_MSI_ENABLE;
	if (drv_pci_device_config_write16(device,
	    cookie->capability + PCI_MSI_CONTROL, control) != 0 ||
	    drv_pci_device_config_write32(device,
	    cookie->capability + PCI_MSI_ADDRESS, (uint32_t)address) != 0) {
		error = EIO;
		goto fail;
	}
	if ((control & PCI_MSI_64BIT) != 0) {
		if (drv_pci_device_config_write32(device,
		    cookie->capability + PCI_MSI_ADDRESS + 4U,
		    (uint32_t)((uint64_t)address >> 32)) != 0) {
			error = EIO;
			goto fail;
		}
		data_offset = cookie->capability + 12U;
	} else {
		if (address > UINT32_MAX) {
			error = ERANGE;
			goto fail;
		}
		data_offset = cookie->capability + 8U;
	}
	if (drv_pci_device_config_write16(device, data_offset,
	    (uint16_t)event) != 0 || drv_pci_device_config_write16(device,
	    cookie->capability + PCI_MSI_CONTROL,
	    control | PCI_MSI_ENABLE) != 0) {
		error = EIO;
		goto fail;
	}
	(void)irq;
	return 0;
fail:
	(void)hal_irq_unregister_msi(cookie->irq);
	return error;
}

static int
map_msix_entry(struct pci_irq_cookie *cookie)
{
	struct drv_pci_device *device = cookie->device;
	struct drv_pci_bar bar;
	uint32_t table;
	uint64_t offset;
	unsigned bir;
	if (drv_pci_device_config_read32(device,
	    cookie->capability + PCI_MSIX_TABLE, &table) != 0)
		return EIO;
	bir = table & 7U;
	offset = (uint64_t)(table & ~7U) +
	    (uint64_t)cookie->index * PCI_MSIX_ENTRY_SIZE;
	if (bir >= device->bar_count ||
	    drv_pci_device_bar(device, bir, &bar) != 0 ||
	    offset > bar.size || PCI_MSIX_ENTRY_SIZE > bar.size - offset)
		return EINVAL;
	bar.bus_address += offset;
	bar.size = PCI_MSIX_ENTRY_SIZE;
	if (device->bus->ops->map_bar == NULL)
		return ENOTSUP;
	return device->bus->ops->map_bar(device->bus->host, device, &bar,
	    DRV_PCI_MAP_READ | DRV_PCI_MAP_WRITE | DRV_PCI_MAP_NOCACHE,
	    &cookie->table);
}

static int
establish_msix(struct pci_irq_cookie *cookie)
{
	volatile uint32_t *entry;
	char source[17];
	paddr_t address;
	uint32_t event;
	uint16_t control;
	int error;

	error = map_msix_entry(cookie);
	if (error != 0)
		return error;
	cookie->table_mapped = 1;
	entry = cookie->table.address;
	entry[3] |= PCI_MSIX_ENTRY_MASK;
	pci_source(&cookie->device->address, source);
	error = hal_irq_register_msi(source, pci_irq_dispatch, cookie,
	    &cookie->irq, &address, &event);
	if (error != HAL_OK) {
		error = error == HAL_ERR_NOMEM ? ENOMEM : EIO;
		goto fail;
	}
	entry[0] = (uint32_t)address;
	entry[1] = (uint32_t)((uint64_t)address >> 32);
	entry[2] = event;
	hal_io_mb();
	if (drv_pci_device_config_read16(cookie->device,
	    cookie->capability + PCI_MSIX_CONTROL, &control) != 0 ||
	    drv_pci_device_config_write16(cookie->device,
	    cookie->capability + PCI_MSIX_CONTROL,
	    (control | PCI_MSIX_ENABLE) &
	    (uint16_t)~PCI_MSIX_FUNCTION_MASK) != 0) {
		error = EIO;
		(void)hal_irq_unregister_msi(cookie->irq);
		goto fail;
	}
	entry[3] &= ~PCI_MSIX_ENTRY_MASK;
	hal_io_mb();
	return 0;
fail:
	if (cookie->table_mapped) {
		cookie->device->bus->ops->unmap_bar(cookie->device->bus->host,
		    &cookie->table);
		cookie->table_mapped = 0;
	}
	return error;
}

int
drv_pci_device_establish_irq(struct drv_pci_device *device,
	const struct drv_pci_irq *irq, drv_pci_irq_handler_t handler,
	void *argument, const char *name, void **result)
{
	struct pci_irq_cookie *cookie;
	int error;
	(void)name;
	if (device == NULL || irq == NULL || handler == NULL || result == NULL)
		return EINVAL;
	cookie = hal_malloc(sizeof(*cookie));
	if (cookie == NULL)
		return ENOMEM;
	memset(cookie, 0, sizeof(*cookie));
	cookie->device = device;
	cookie->type = irq->type;
	cookie->capability = (unsigned)irq->private_data[0];
	cookie->index = irq->index;
	cookie->handler = handler;
	cookie->argument = argument;
	if (irq->type == DRV_PCI_IRQ_INTX) {
		cookie->irq = (int)irq->vector;
		error = hal_irq_set_handler(cookie->irq, pci_irq_dispatch, cookie) ==
		    HAL_OK ? 0 : EIO;
		if (error == 0)
			hal_irq_unmask(cookie->irq);
	} else if (irq->type == DRV_PCI_IRQ_MSI) {
		error = establish_msi(cookie, irq);
	} else if (irq->type == DRV_PCI_IRQ_MSIX) {
		error = establish_msix(cookie);
	} else {
		error = EINVAL;
	}
	if (error != 0) {
		hal_free(cookie);
		return error;
	}
	*result = cookie;
	return 0;
}

void
drv_pci_device_disestablish_irq(struct drv_pci_device *device, void *value)
{
	struct pci_irq_cookie *cookie = value;
	uint16_t control;
	(void)device;
	if (cookie == NULL)
		return;
	if (cookie->type == DRV_PCI_IRQ_INTX) {
		hal_irq_mask(cookie->irq);
		(void)hal_irq_set_handler(cookie->irq, NULL, NULL);
	} else if (cookie->type == DRV_PCI_IRQ_MSI) {
		if (drv_pci_device_config_read16(cookie->device,
		    cookie->capability + PCI_MSI_CONTROL, &control) == 0)
			(void)drv_pci_device_config_write16(cookie->device,
			    cookie->capability + PCI_MSI_CONTROL,
			    control & (uint16_t)~PCI_MSI_ENABLE);
		(void)hal_irq_unregister_msi(cookie->irq);
	} else if (cookie->type == DRV_PCI_IRQ_MSIX) {
		if (cookie->table_mapped) {
			volatile uint32_t *entry = cookie->table.address;
			entry[3] |= PCI_MSIX_ENTRY_MASK;
			hal_io_mb();
		}
		(void)hal_irq_unregister_msi(cookie->irq);
		if (cookie->table_mapped)
			cookie->device->bus->ops->unmap_bar(
			    cookie->device->bus->host, &cookie->table);
	}
	hal_free(cookie);
}

struct drv_dma_device*drv_pci_device_dma(struct drv_pci_device*d){return d?d->bus->dma:NULL;}
struct drv_pci_driver*drv_pci_device_driver(const struct drv_pci_device*d){return d?d->driver:NULL;}
void*drv_pci_device_driver_data(const struct drv_pci_device*d){return d?d->driver_data:NULL;}
int drv_pci_device_set_driver_data(struct drv_pci_device*d,void*p){if(!d)return EINVAL;d->driver_data=p;return 0;}
int drv_pci_id_match(const struct drv_pci_id*i,const struct drv_pci_device*d){if(!i||!d)return 0;return(i->vendor==DRV_PCI_ANY_ID||i->vendor==d->vendor)&&(i->device==DRV_PCI_ANY_ID||i->device==d->product)&&(i->subvendor==DRV_PCI_ANY_ID||i->subvendor==d->subvendor)&&(i->subdevice==DRV_PCI_ANY_ID||i->subdevice==d->subproduct)&&((d->class_code&i->class_mask)==(i->class_code&i->class_mask));}
const struct drv_pci_id*drv_pci_driver_find_id(const struct drv_pci_driver*r,const struct drv_pci_device*d){size_t n;if(!r)return NULL;for(n=0;n<r->id_count;n++)if(drv_pci_id_match(&r->ids[n],d))return&r->ids[n];return NULL;}
int drv_pci_driver_match(struct drv_pci_driver*r,struct drv_pci_device*d,const struct drv_pci_id**out){const struct drv_pci_id*i=drv_pci_driver_find_id(r,d);int score;if(!i)return DRV_PCI_MATCH_NONE;score=r->match?r->match(d,i):DRV_PCI_MATCH_GENERIC;if(score>0&&out)*out=i;return score;}
int drv_pci_device_probe(struct drv_pci_device*d){struct pci_driver_entry*e,*best=NULL;const struct drv_pci_id*i,*best_id=NULL;int score,best_score=0,error;if(!d||d->driver)return d?EBUSY:EINVAL;for(e=drivers;e;e=e->next)if((score=drv_pci_driver_match(e->driver,d,&i))>best_score){best=e;best_id=i;best_score=score;}if(!best)return ENODEV;error=best->driver->attach?best->driver->attach(d,best_id):0;if(error)return error;d->driver=best->driver;return 0;}
int drv_pci_device_detach(struct drv_pci_device*d,unsigned f){int e=0;if(!d||!d->driver)return EINVAL;if(d->driver->detach)e=d->driver->detach(d,f);if(!e){d->driver=NULL;d->driver_data=NULL;}return e;}
int drv_pci_device_reprobe(struct drv_pci_device*d){if(!d)return EINVAL;if(d->driver){int e=drv_pci_device_detach(d,0);if(e)return e;}return drv_pci_device_probe(d);}
int drv_pci_driver_register(struct drv_pci_driver*r){struct pci_driver_entry*e,**p;if(!initialized||!r||!r->name)return EINVAL;for(e=drivers;e;e=e->next)if(e->driver==r)return EEXIST;e=hal_malloc(sizeof(*e));if(!e)return ENOMEM;e->driver=r;e->next=NULL;for(p=&drivers;*p;p=&(*p)->next);*p=e;{struct drv_pci_bus*b;struct drv_pci_device*d;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)if(!d->driver)(void)drv_pci_device_probe(d);}return 0;}
int drv_pci_driver_unregister(struct drv_pci_driver*r){struct pci_driver_entry**p,*e;struct drv_pci_bus*b;struct drv_pci_device*d;if(!r)return EINVAL;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)if(d->driver==r&&drv_pci_device_detach(d,0))return EBUSY;for(p=&drivers;(e=*p)!=NULL;p=&e->next)if(e->driver==r){*p=e->next;hal_free(e);return 0;}return ENOENT;}
const char*drv_pci_driver_name(const struct drv_pci_driver*r){return r?r->name:NULL;}
size_t drv_pci_driver_device_count(const struct drv_pci_driver*r){size_t n=0;struct drv_pci_bus*b;struct drv_pci_device*d;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)if(d->driver==r)n++;return n;}
int drv_pci_driver_foreach_device(struct drv_pci_driver*r,drv_pci_device_iterator_t fn,void*a){struct drv_pci_bus*b;struct drv_pci_device*d;int e;if(!r||!fn)return EINVAL;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)if(d->driver==r&&(e=fn(d,a))!=0)return e;return 0;}
void drv_pci_dump(void){struct drv_pci_bus*b;struct drv_pci_device*d;for(b=root_buses;b;b=b->next)for(d=b->devices;d;d=d->next)hal_printf("pci: %04x:%02x:%02x.%u %04x:%04x class %06x%s%s\n",d->address.segment,d->address.bus,d->address.device,d->address.function,d->vendor,d->product,d->class_code,d->driver?" driver=":"",d->driver?d->driver->name:"");}
