/* ws004-p043 amd64 hal_printf conversion regression.
 *
 * The q070 physical RTL8822BU candidate printed its connect diagnostics as
 * literal "%llu" because the production hal_printf implemented no length
 * modifiers while the focused-suite hal_printf stub discarded output.  This
 * gate formats through the real amd64 HAL implementation into a captured
 * console and locks both the original conversions and the added l/ll forms.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <hal/hal.h>

#define CAPTURE_SIZE 512U

static char capture[CAPTURE_SIZE];
static size_t capture_length;
static unsigned output_tokens_open;
static unsigned output_records;

/* Captures one console byte for later comparison. */
void
hal_cons_putc(int c)
{
	assert(output_tokens_open == 1U);
	assert(capture_length < CAPTURE_SIZE - 1U);
	capture[capture_length++] = (char)c;
	capture[capture_length] = '\0';
}

/* Captures one console string for later comparison. */
void
hal_cons_write(const char *utf8)
{
	while (*utf8 != '\0')
		hal_cons_putc(*utf8++);
}

/* Serializes one formatted record exactly like the BSP console. */
uint64_t
pcat_cons_output_begin(void)
{
	assert(output_tokens_open == 0U);
	output_tokens_open = 1U;
	return 0x5a5aU;
}

/* Releases the serialized record token. */
void
pcat_cons_output_end(uint64_t token)
{
	assert(token == 0x5a5aU);
	assert(output_tokens_open == 1U);
	output_tokens_open = 0U;
	output_records++;
}

/* The panic paths must never run during formatting. */
void
asm_cli(void)
{
	assert(0);
}

/* The panic paths must never run during formatting. */
void
asm_hlt(void)
{
	assert(0);
}

/* The panic paths must never run during formatting. */
int
amd64_smp_panic_available(void)
{
	assert(0);
	return 0;
}

/* The panic paths must never run during formatting. */
void
hal_cpu_panic_all(void)
{
	assert(0);
}

/* Formats one record and asserts the exact captured console bytes. */
static void
expect(const char *expected, int actual)
{
	assert(actual == 0);
	if (strcmp(capture, expected) != 0) {
		fprintf(stderr, "expected \"%s\" got \"%s\"\n", expected,
		    capture);
		assert(0);
	}
	capture_length = 0U;
	capture[0] = '\0';
}

int
main(void)
{
	/* The original conversions must format exactly as before. */
	expect("plain text", hal_printf("plain text"));
	expect("x=7;", hal_printf("x=%d;", 7));
	expect("x=-7;", hal_printf("x=%d;", -7));
	expect("x=-2147483648;", hal_printf("x=%d;", INT32_MIN));
	expect("u=4294967295;", hal_printf("u=%u;", UINT32_MAX));
	expect("hex=00beef", hal_printf("hex=%06x", 0xbeefU));
	expect("HEX=BEEF", hal_printf("HEX=%X", 0xbeefU));
	expect("pad=   42", hal_printf("pad=%5u", 42U));
	expect("c=Q", hal_printf("c=%c", 'Q'));
	expect("s=abc", hal_printf("s=%s", "abc"));
	expect("s=(null)", hal_printf("s=%s", (const char *)NULL));
	expect("100%", hal_printf("100%%"));

	/* Unknown conversions still pass through unconsumed. */
	expect("%p", hal_printf("%p"));
	expect("%q!", hal_printf("%q!"));
	expect("%", hal_printf("%"));

	/* The added l and ll modifiers format full 64-bit magnitudes. */
	expect("g=18446744073709551615;",
	    hal_printf("g=%llu;", (unsigned long long)UINT64_MAX));
	expect("g=ffffffffffffffff;",
	    hal_printf("g=%llx;", (unsigned long long)UINT64_MAX));
	expect("g=FFFFFFFFFFFFFFFF;",
	    hal_printf("g=%llX;", (unsigned long long)UINT64_MAX));
	expect("g=0;", hal_printf("g=%llu;", 0ULL));
	expect("g=10;", hal_printf("g=%llu;", 10ULL));
	expect("g=4294967295;", hal_printf("g=%llu;", 4294967295ULL));
	expect("g=4294967296;", hal_printf("g=%llu;", 4294967296ULL));
	expect("g=826608432;", hal_printf("g=%llu;", 826608432ULL));
	expect("g=123456789012345678;",
	    hal_printf("g=%llu;", 123456789012345678ULL));
	expect("d=-9223372036854775808;",
	    hal_printf("d=%lld;", (long long)INT64_MIN));
	expect("d=9223372036854775807;",
	    hal_printf("d=%lld;", (long long)INT64_MAX));
	expect("l=18446744073709551615;",
	    hal_printf("l=%lu;", (unsigned long)UINT64_MAX));
	expect("l=deadbeefcafe;", hal_printf("l=%lx;",
	    (unsigned long)0xdeadbeefcafeUL));
	expect("d=-1;", hal_printf("d=%ld;", -1L));
	expect("w=00000000004294967296",
	    hal_printf("w=%020llu", 4294967296ULL));
	expect("w=          4294967296",
	    hal_printf("w=%20llu", 4294967296ULL));

	/* Unknown conversions restore consumed length modifiers verbatim. */
	expect("%lp", hal_printf("%lp"));
	expect("%llp", hal_printf("%llp"));
	expect("%ll", hal_printf("%ll"));

	/* Mixed argument order must stay aligned across widths. */
	expect("a=1 b=8589934592 c=two d=3",
	    hal_printf("a=%u b=%llu c=%s d=%d", 1U, 8589934592ULL, "two", 3));

	assert(output_tokens_open == 0U);
	assert(output_records != 0U);
	printf("hal printf format fixture: PASS\n");
	return 0;
}
