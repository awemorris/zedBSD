#ifndef ZEDBSD_HAL_SPARCV9_SPACE_H
#define ZEDBSD_HAL_SPARCV9_SPACE_H

#include <hal/hal.h>

#define SPARCV9_SPACE_MAGIC 0x53395639U
struct sparcv9_mapping { uintptr_t virtual_address,physical_address;uint32 attributes,flags;struct sparcv9_mapping*next; };
struct sparcv9_space { uint32 magic,context;struct sparcv9_mapping*mappings; };
void sparcv9_page_init(void);
void sparcv9_space_init(void);
uintptr_t sparcv9_direct_to_phys(const void *address);
void *sparcv9_phys_to_direct(uintptr_t address);
int sparcv9_resolve_miss(uintptr_t address, int instruction, int write);
int sparcv9_prime_mapping(uintptr_t address, int instruction, int write);

#endif
