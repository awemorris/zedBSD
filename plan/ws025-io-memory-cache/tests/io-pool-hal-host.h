/* Shared HAL allocation model for production I/O pool fixtures. */
#ifndef WS025_IO_POOL_HAL_HOST_H
#define WS025_IO_POOL_HAL_HOST_H
static size_t ram = 256U * 1024U * 1024U;
static size_t page_size = 4096;
static size_t live_bytes;
static unsigned allocation_calls, fail_at, cpus = 4;

void hal_pmem_get_stats(struct hal_pmem_stats *stats)
{ memset(stats, 0, sizeof(*stats)); stats->physical_total = ram; }
unsigned hal_cpu_count(void) { return cpus; }
size_t hal_space_get_page_size(int level) { return level == 1 ? page_size : 0; }
void *hal_memset(void *p, int value, size_t size) { return memset(p, value, size); }
int hal_printf(const char *format, ...) { (void)format; return 0; }
void hal_fatal(const char *file, int line, const char *message)
{ fprintf(stderr, "%s:%d %s\n", file, line, message); abort(); }
int hal_pmem_alloc(hal_physaddr_t request_paddr, size_t request_size, size_t request_alignment, uint32_t request_type, uint32_t request_attr, struct hal_pmem *memory)
{
	(void)request_paddr; (void)request_attr;

	allocation_calls++;
	if (allocation_calls == fail_at) return HAL_ERR_NOMEM;
	assert(request_size % page_size == 0 && request_alignment == page_size);
	memory->vaddr = aligned_alloc(page_size, request_size);
	assert(memory->vaddr);
	memory->paddr = (uintptr_t)memory->vaddr;
	memory->size = request_size;
	memory->type = request_type;
	memory->attr = 0;
	live_bytes += memory->size;
	return HAL_OK;
}
int hal_pmem_free(struct hal_pmem *memory)
{ live_bytes -= memory->size; free(memory->vaddr); memset(memory, 0, sizeof(*memory)); return HAL_OK; }


#endif
