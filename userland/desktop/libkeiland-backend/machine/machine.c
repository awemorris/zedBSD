/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The system's names as Settings' About shows them (ws188-p002, moved
 * from Settings' about.c of ws089-p027): the version's name (PRETTY_NAME
 * of /etc/os-release, or of /usr/lib/os-release when /etc has none), the
 * kernel and the architecture (uname), the processor's name (the CPU's own
 * brand string, on x86), how many processors are online, and the
 * computer's name.
 *
 * These are POSIX interfaces and the processor's instruction, the same on
 * zedBSD, Linux and FreeBSD, so they are shared by the three operating
 * systems' backends.  The compositor calls them on its machine thread.
 */

#include "userland/desktop/libkeiland-backend/backend-private.h"

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

/* The longest os-release line read. */
#define MACHINE_LINE_MAX	256U

/* The brand string's leaves, and its length (three leaves of four registers of four bytes). */
#define MACHINE_BRAND_LEAF_MAX	0x80000000U
#define MACHINE_BRAND_LEAF	0x80000002U
#define MACHINE_BRAND_LAST	0x80000004U
#define MACHINE_BRAND_WORDS	12U

static void machine_processor(char *name, size_t size);
static void machine_unquote(char *value);
#if defined(__x86_64__) || defined(__i386__)
static void machine_trim(char *text);
#endif

/*
 * Reads the system's names; a value that cannot be read is left empty.
 */
int
kl_backend_machine_read(
	struct kl_backend_machine *machine)
{
	struct utsname names;
	char text[KL_BACKEND_MACHINE_SYSTEM + KL_BACKEND_MACHINE_KERNEL];
	long cores;
	int status;

	/* Nothing known yet. */
	memset(machine, 0, sizeof(*machine));

	/* The system's version, from /etc/os-release or the system's own copy. */
	status = kl_backend_machine_pretty_name("/etc/os-release", machine->system, sizeof(machine->system));
	if (status != 0)
		(void)kl_backend_machine_pretty_name("/usr/lib/os-release", machine->system, sizeof(machine->system));

	/* The kernel's name and release, and the architecture. */
	status = uname(&names);
	if (status == 0) {
		(void)snprintf(text, sizeof(text), "%s %s", names.sysname, names.release);
		kl_backend_machine_copy(machine->kernel, sizeof(machine->kernel), text, strlen(text));
		kl_backend_machine_copy(machine->architecture, sizeof(machine->architecture), names.machine, strlen(names.machine));
	}

	/* The processor's name. */
	machine_processor(machine->processor, sizeof(machine->processor));

	/* The processors online. */
	cores = sysconf(_SC_NPROCESSORS_ONLN);
	if (cores > 0)
		machine->cpus = (unsigned)cores;

	/* The computer's name; one that fills the room has no NUL of its own. */
	status = gethostname(machine->host, sizeof(machine->host) - 1U);
	if (status != 0)
		machine->host[0] = '\0';
	machine->host[sizeof(machine->host) - 1U] = '\0';

	/* Succeeded: what could be read is filled. */
	return 0;
}

/*
 * Reads PRETTY_NAME of an os-release file (os-release(5): KEY=value lines,
 * the value maybe in double or single quotes, with backslash escapes in
 * double quotes, "#" starting a comment).  Returns 0 with the name, cut
 * short at a character's boundary to fit, or -1 when the file cannot be
 * read or has no PRETTY_NAME (the name is then empty).
 */
int
kl_backend_machine_pretty_name(
	const char *path,
	char *name,
	size_t size)
{
	char line[MACHINE_LINE_MAX];
	FILE *file;
	char *read;
	char *value;
	size_t length;
	int same;
	int found;

	/* Nothing yet. */
	name[0] = '\0';

	/* The file. */
	file = fopen(path, "r");
	if (file == NULL)
		return -1;

	/* Each line, until PRETTY_NAME. */
	found = -1;
	for (;;) {
		/* The next line; the file's end ends the search. */
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;

		/* The line without its end. */
		length = strcspn(line, "\r\n");
		line[length] = '\0';

		/* Only PRETTY_NAME= at the line's start counts. */
		same = strncmp(line, "PRETTY_NAME=", 12U);
		if (same != 0)
			continue;

		/* Its value, without the quotes. */
		value = line + 12;
		machine_unquote(value);
		kl_backend_machine_copy(name, size, value, strlen(value));
		found = 0;
		break;
	}

	/* The file is no longer needed. */
	(void)fclose(file);

	/* An empty name counts as none. */
	if (found == 0 && name[0] == '\0')
		found = -1;

	/* Reports a file without a name. */
	if (found != 0)
		return -1;

	/* Succeeded: the name is copied. */
	return 0;
}

/*
 * Copies length bytes of a UTF-8 text into a room of size bytes with its
 * NUL; a text that does not fit is cut before the character that would
 * not fit whole, never inside one.
 */
void
kl_backend_machine_copy(
	char *to,
	size_t size,
	const char *from,
	size_t length)
{
	unsigned char byte;

	/* No room at all. */
	if (size == 0U)
		return;

	/* A text that fits is copied whole. */
	if (length < size) {
		memcpy(to, from, length);
		to[length] = '\0';
		return;
	}

	/* Too long: back off from the room's end while the byte there continues a character. */
	length = size - 1U;
	while (length > 0U) {
		/* A continuation byte (10xxxxxx) is not where a character starts. */
		byte = (unsigned char)from[length];
		if ((byte & 0xc0U) != 0x80U)
			break;
		length--;
	}

	/* The whole characters before the cut. */
	memcpy(to, from, length);
	to[length] = '\0';
}

/*
 * Reads the processor's brand string with CPUID (leaves 0x80000002 to
 * 0x80000004), on x86 only; elsewhere, or when the processor has none,
 * the name stays empty.
 */
static void
machine_processor(
	char *name,
	size_t size)
{
#if defined(__x86_64__) || defined(__i386__)
	uint32_t words[MACHINE_BRAND_WORDS + 1U];
	uint32_t leaf;
	uint32_t highest;
	uint32_t eax;
	uint32_t ebx;
	uint32_t ecx;
	uint32_t edx;
	unsigned index;

	/* Nothing yet. */
	name[0] = '\0';

	/* The highest extended leaf the processor answers. */
	leaf = MACHINE_BRAND_LEAF_MAX;
	__asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(0U));
	highest = eax;
	if (highest < MACHINE_BRAND_LAST)
		return;

	/* The three leaves of the brand string, sixteen characters each. */
	for (index = 0; index < 3U; index++) {
		leaf = MACHINE_BRAND_LEAF + index;
		__asm__ __volatile__("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf), "c"(0U));
		words[4U * index] = eax;
		words[4U * index + 1U] = ebx;
		words[4U * index + 2U] = ecx;
		words[4U * index + 3U] = edx;
	}

	/* The string ended after its 48 bytes, without the padding spaces, then copied. */
	words[MACHINE_BRAND_WORDS] = 0U;
	machine_trim((char *)words);
	kl_backend_machine_copy(name, size, (const char *)words, strlen((const char *)words));
#else
	/* No brand string on this architecture. */
	(void)size;
	name[0] = '\0';
#endif
}

#if defined(__x86_64__) || defined(__i386__)
/* Takes the spaces off a text's start and end, and runs of spaces inside it down to one. */
static void
machine_trim(
	char *text)
{
	size_t read;
	size_t written;
	int space;

	/* Copies the text over itself, keeping one space between words. */
	written = 0;
	space = 0;
	for (read = 0; text[read] != '\0'; read++) {
		/* A space is kept only before a word that follows it. */
		if (text[read] == ' ') {
			space = 1;
			continue;
		}

		/* A word after a space gets one space, unless it starts the text. */
		if (space != 0 && written > 0) {
			text[written] = ' ';
			written++;
		}

		/* The word's character. */
		space = 0;
		text[written] = text[read];
		written++;
	}

	/* The text ends after its last word. */
	text[written] = '\0';
}
#endif

/*
 * Takes the quotes off an os-release value in place: within double quotes
 * a backslash keeps the character after it (\", \\, \$, \`); within
 * single quotes everything is kept; a value without quotes ends at the
 * first space.
 */
static void
machine_unquote(
	char *value)
{
	size_t read;
	size_t written;
	char quote;

	/* A value without quotes: up to its first space. */
	quote = value[0];
	if (quote != '"' && quote != '\'') {
		written = strcspn(value, " \t");
		value[written] = '\0';
		return;
	}

	/* Inside the quotes, up to the closing one. */
	written = 0;
	for (read = 1; value[read] != '\0' && value[read] != quote; read++) {
		/* A backslash in double quotes keeps the next character. */
		if (quote == '"' && value[read] == '\\' && value[read + 1U] != '\0')
			read++;

		/* The character. */
		value[written] = value[read];
		written++;
	}

	/* The value ends where it was copied to. */
	value[written] = '\0';
}
