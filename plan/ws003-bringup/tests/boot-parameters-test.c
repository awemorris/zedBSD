/* Common boot-parameter parser regression fixture (BR-T42). */
#include <kern/boot-parameters.h>
#include <kern/init.h>

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

struct process;

static unsigned spawn_count;
static const char *spawn_path;
static int spawn_result;

int
process_spawn_init(const char *path, struct process **result)
{
	assert(result == NULL);
	spawn_count++;
	spawn_path = path;
	return spawn_result;
}

static void
parse_ok(struct kern_boot_parameters *parameters, const char *input)
{
	assert(kern_boot_parameters_parse(parameters, input,
	    strlen(input) + 1U) == 0);
}

static void
test_defaults_and_complete_name_set(void)
{
	struct kern_boot_parameters parameters;
	const char *all =
	    "boot0=b0 boot1=b1 boot2=b2 boot3=b3 rootpart=root "
	    "overlay-root=lower overlay-data=upper "
	    "swap0=s0 swap1=s1 swap2=s2 swap3=s3 init=/custom/init";

	assert(kern_boot_parameters_parse(&parameters, NULL, 0) == 0);
	assert(strcmp(kern_boot_parameters_init_path(&parameters),
	    "/sbin/init") == 0);
	parse_ok(&parameters, "");
	assert(strcmp(kern_boot_parameters_init_path(&parameters),
	    "/sbin/init") == 0);
	parse_ok(&parameters, "   ");
	assert(strcmp(kern_boot_parameters_init_path(&parameters),
	    "/sbin/init") == 0);

	parse_ok(&parameters, all);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 0), "b0") == 0);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 1), "b1") == 0);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 2), "b2") == 0);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 3), "b3") == 0);
	assert(kern_boot_parameters_boot(&parameters, 4) == NULL);
	assert(strcmp(kern_boot_parameters_rootpart(&parameters), "root") == 0);
	assert(strcmp(kern_boot_parameters_overlay_root(&parameters),
	    "lower") == 0);
	assert(strcmp(kern_boot_parameters_overlay_data(&parameters),
	    "upper") == 0);
	assert(strcmp(kern_boot_parameters_swap(&parameters, 0), "s0") == 0);
	assert(strcmp(kern_boot_parameters_swap(&parameters, 1), "s1") == 0);
	assert(strcmp(kern_boot_parameters_swap(&parameters, 2), "s2") == 0);
	assert(strcmp(kern_boot_parameters_swap(&parameters, 3), "s3") == 0);
	assert(kern_boot_parameters_swap(&parameters, 4) == NULL);
	assert(strcmp(kern_boot_parameters_init_path(&parameters),
	    "/custom/init") == 0);
	assert(kern_boot_parameters_unknown_count(&parameters) == 0U);
}

static void
test_sparse_indices_and_owned_storage(void)
{
	struct kern_boot_parameters parameters;
	char input[] = "  boot1=one   boot3=three swap2=two  ";

	parse_ok(&parameters, input);
	assert(kern_boot_parameters_boot(&parameters, 0) == NULL);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 1), "one") == 0);
	assert(kern_boot_parameters_boot(&parameters, 2) == NULL);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 3), "three") ==
	    0);
	assert(kern_boot_parameters_swap(&parameters, 0) == NULL);
	assert(kern_boot_parameters_swap(&parameters, 1) == NULL);
	assert(strcmp(kern_boot_parameters_swap(&parameters, 2), "two") == 0);
	assert(kern_boot_parameters_swap(&parameters, 3) == NULL);

	memset(input, 'x', sizeof(input) - 1U);
	input[sizeof(input) - 1U] = '\0';
	assert(strcmp(kern_boot_parameters_boot(&parameters, 1), "one") == 0);
	assert(strcmp(kern_boot_parameters_boot(&parameters, 3), "three") ==
	    0);
	parse_ok(&parameters, "boot0=UUID=6740-911D");
	assert(strcmp(kern_boot_parameters_boot(&parameters, 0),
	    "UUID=6740-911D") == 0);
}

static void
test_unknown_names(void)
{
	struct kern_boot_parameters parameters;
	char input[96];
	const char *name;
	int truncated;

	parse_ok(&parameters,
	    "future=value init=/bin/sh another=x BOOT0=case-sensitive");
	assert(kern_boot_parameters_unknown_count(&parameters) == 3U);
	name = kern_boot_parameters_unknown_name(&parameters, &truncated);
	assert(strcmp(name, "future") == 0);
	assert(!truncated);
	assert(strcmp(kern_boot_parameters_init_path(&parameters), "/bin/sh") ==
	    0);

	memset(input, 'a', 40U);
	memcpy(input + 40U, "=value second=x", 16U);
	input[56] = '\0';
	parse_ok(&parameters, input);
	assert(kern_boot_parameters_unknown_count(&parameters) == 2U);
	name = kern_boot_parameters_unknown_name(&parameters, &truncated);
	assert(strlen(name) == KERN_BOOT_PARAMETERS_UNKNOWN_NAME_MAX);
	assert(truncated);

	parse_ok(&parameters, "future=a=b future=c");
	assert(kern_boot_parameters_unknown_count(&parameters) == 2U);
}

static void
test_duplicate_known_names(void)
{
	static const char *const names[] = {
		"boot0", "boot1", "boot2", "boot3", "rootpart",
		"overlay-root", "overlay-data", "swap0", "swap1",
		"swap2", "swap3", "init"
	};
	struct kern_boot_parameters parameters;
	char input[96];

	for (size_t index = 0; index < sizeof(names) / sizeof(names[0]);
	     index++) {
		if (strcmp(names[index], "init") == 0)
			assert(snprintf(input, sizeof(input), "%s=/first %s=/second",
			    names[index], names[index]) > 0);
		else
			assert(snprintf(input, sizeof(input), "%s=first %s=second",
			    names[index], names[index]) > 0);
		assert(kern_boot_parameters_parse(&parameters, input,
		    strlen(input) + 1U) == EEXIST);
		assert(kern_boot_parameters_boot(&parameters, 0) == NULL);
		assert(strcmp(kern_boot_parameters_init_path(&parameters),
		    "/sbin/init") == 0);
	}
}

static void
test_malformed_input(void)
{
	static const char *const malformed[] = {
		"boot0", "=value", "boot0=", "future=", "boot0 value=x",
		"boot0=x\tinit=/sbin/init", "boot0=x\ninit=/sbin/init",
		"boot0=x\177"
	};
	struct kern_boot_parameters parameters;
	const char short_unterminated[] = {'i', 'n', 'i', 't', '=', '/', 'x'};
	const char non_ascii[] = {'b', 'o', 'o', 't', '0', '=', 'x',
		(char)0x80, '\0'};
	const char valid[] = "boot0=value";

	assert(kern_boot_parameters_parse(NULL, "", 1U) == EINVAL);
	assert(kern_boot_parameters_parse(&parameters, NULL, 1U) == EINVAL);
	assert(kern_boot_parameters_parse(&parameters, valid, 0U) == EINVAL);
	assert(kern_boot_parameters_parse(&parameters, valid,
	    sizeof(valid) - 1U) == EINVAL);
	assert(kern_boot_parameters_parse(&parameters, short_unterminated,
	    sizeof(short_unterminated)) == EINVAL);
	assert(kern_boot_parameters_parse(&parameters, non_ascii,
	    sizeof(non_ascii)) == EILSEQ);
	for (size_t index = 0;
	     index < sizeof(malformed) / sizeof(malformed[0]); index++)
		assert(kern_boot_parameters_parse(&parameters, malformed[index],
		    strlen(malformed[index]) + 1U) == EINVAL);
}

static void
test_exact_text_limits(void)
{
	struct kern_boot_parameters parameters;
	char exact[KERN_BOOT_PARAMETERS_STORAGE_SIZE];
	char over[KERN_BOOT_PARAMETERS_STORAGE_SIZE + 1U];
	char missing[KERN_BOOT_PARAMETERS_STORAGE_SIZE];

	memcpy(exact, "boot0=", 6U);
	memset(exact + 6U, 'a', KERN_BOOT_PARAMETERS_TEXT_MAX - 6U);
	exact[KERN_BOOT_PARAMETERS_TEXT_MAX] = '\0';
	assert(kern_boot_parameters_parse(&parameters, exact, sizeof(exact)) ==
	    0);
	assert(strlen(kern_boot_parameters_boot(&parameters, 0)) ==
	    KERN_BOOT_PARAMETERS_TEXT_MAX - 6U);

	memcpy(over, "boot0=", 6U);
	memset(over + 6U, 'a', KERN_BOOT_PARAMETERS_TEXT_MAX - 5U);
	over[KERN_BOOT_PARAMETERS_STORAGE_SIZE] = '\0';
	assert(kern_boot_parameters_parse(&parameters, over, sizeof(over)) ==
	    E2BIG);

	memset(missing, 'x', sizeof(missing));
	assert(kern_boot_parameters_parse(&parameters, missing,
	    sizeof(missing)) == E2BIG);
}

static void
test_init_validation(void)
{
	struct kern_boot_parameters parameters;
	char accepted[5U + KERN_BOOT_PARAMETERS_INIT_PATH_MAX + 1U];
	char rejected[5U + KERN_BOOT_PARAMETERS_INIT_PATH_MAX + 2U];

	parse_ok(&parameters, "init=/sbin/init");
	assert(strcmp(kern_boot_parameters_init_path(&parameters),
	    "/sbin/init") == 0);
	parse_ok(&parameters, "init=/bin/sh");
	assert(strcmp(kern_boot_parameters_init_path(&parameters), "/bin/sh") ==
	    0);
	parse_ok(&parameters, "init=/");
	assert(strcmp(kern_boot_parameters_init_path(&parameters), "/") == 0);
	assert(kern_boot_parameters_parse(&parameters, "init=bin/sh",
	    sizeof("init=bin/sh")) == EINVAL);

	memcpy(accepted, "init=/", 6U);
	memset(accepted + 6U, 'a', KERN_BOOT_PARAMETERS_INIT_PATH_MAX - 1U);
	accepted[5U + KERN_BOOT_PARAMETERS_INIT_PATH_MAX] = '\0';
	assert(kern_boot_parameters_parse(&parameters, accepted,
	    sizeof(accepted)) == 0);
	assert(strlen(kern_boot_parameters_init_path(&parameters)) ==
	    KERN_BOOT_PARAMETERS_INIT_PATH_MAX);

	memcpy(rejected, "init=/", 6U);
	memset(rejected + 6U, 'a', KERN_BOOT_PARAMETERS_INIT_PATH_MAX);
	rejected[6U + KERN_BOOT_PARAMETERS_INIT_PATH_MAX] = '\0';
	assert(kern_boot_parameters_parse(&parameters, rejected,
	    sizeof(rejected)) == ENAMETOOLONG);
}

static void
test_kernel_global_instance(void)
{
	const struct kern_boot_parameters *parameters;
	const char empty[] = "";
	const char valid[] = "init=/bin/sh";
	const char invalid[] = "init=relative";

	assert(kern_boot_parameters_initialize(NULL, 0U) == 0);
	assert(kern_boot_parameters_current() != NULL);
	assert(!kern_boot_parameters_source_present());
	assert(kern_boot_parameters_initialize(empty, sizeof(empty)) == 0);
	assert(kern_boot_parameters_current() != NULL);
	assert(kern_boot_parameters_source_present());
	assert(kern_boot_parameters_initialize(valid, sizeof(valid)) == 0);
	assert(kern_boot_parameters_source_present());
	parameters = kern_boot_parameters_current();
	assert(parameters != NULL);
	assert(strcmp(kern_boot_parameters_init_path(parameters), "/bin/sh") ==
	    0);
	assert(kern_boot_parameters_initialize(invalid, sizeof(invalid)) ==
	    EINVAL);
	assert(kern_boot_parameters_current() == NULL);
	assert(!kern_boot_parameters_source_present());
}

static void
test_init_start_is_single_and_final(void)
{
	spawn_count = 0;
	spawn_path = NULL;
	spawn_result = ENOENT;
	assert(kern_init_start("/selected/missing") == ENOENT);
	assert(spawn_count == 1U);
	assert(strcmp(spawn_path, "/selected/missing") == 0);

	spawn_count = 0;
	spawn_result = 0;
	assert(kern_init_start("/bin/sh") == 0);
	assert(spawn_count == 1U);
	assert(strcmp(spawn_path, "/bin/sh") == 0);
}

int
main(void)
{
	test_defaults_and_complete_name_set();
	test_sparse_indices_and_owned_storage();
	test_unknown_names();
	test_duplicate_known_names();
	test_malformed_input();
	test_exact_text_limits();
	test_init_validation();
	test_kernel_global_instance();
	test_init_start_is_single_and_final();
	puts("BR-T42 boot-parameter parser: PASS");
	return 0;
}
