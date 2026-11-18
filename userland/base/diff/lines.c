/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * Loads files as lines and finds a longest common subsequence of their
 * lines.
 *
 * Every line gets a number shared by the lines equal to it, so that the
 * comparison works on numbers.  The common subsequence is found by the
 * O(ND) algorithm of Myers ("An O(ND) Difference Algorithm and Its
 * Variations", 1986) in its linear-space form: the common start and end
 * of a range are taken off, the middle of an optimal path is found by
 * searching from both ends at once, and the two halves are solved the
 * same way.  Lines that do not occur in the other file at all cannot be
 * in any common subsequence and are set aside first, which keeps files
 * with little in common fast.
 */

#include "userland/base/diff/lines.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The first size of the table that numbers the lines. */
#define LINES_TABLE_SIZE 1024

/*
 * One distinct line in the numbering table.
 *
 * key is the line as compared (with blanks folded under -b); its number
 * is the id the equal lines share.  The table owns the key.
 */
struct line_entry {
	char *key;
	size_t length;
	unsigned long hash;
	long id;
};

/*
 * The table of distinct lines while two files are numbered.
 *
 * It is open-addressed and doubles when half full; it lives only during
 * diff_number_lines().
 */
struct line_table {
	struct line_entry *entries;
	size_t size;
	size_t used;
};

/*
 * The state of one comparison.
 *
 * a and b are the line numbers of the two files; match_a[i] is the line of
 * b that line i of a is matched with, or -1.
 */
struct lcs_state {
	const long *a;
	const long *b;
	long *match_a;
};

/*
 * The arrays of one matching of two files.
 *
 * counts_* count the occurrences of each line id in each file; kept_*
 * hold the ids of the lines the other file also has, and map_* their line
 * numbers; match and result hold the matches of the kept lines and of the
 * whole first file.  They live during match_lines() only, except result,
 * which it hands to its caller.
 */
struct match_work {
	long *counts_a;
	long *counts_b;
	long *kept_a;
	long *kept_b;
	long *map_a;
	long *map_b;
	long *match;
	long *result;
	long kept_a_count;
	long kept_b_count;
};

static int number_file(struct line_table *table, struct diff_file *file, int fold_blanks);
static long line_id(struct line_table *table, const char *text, size_t length, int fold_blanks, int unterminated, int *failed);
static size_t fold_line(const char *text, size_t length, char *out);
static unsigned long hash_bytes(const char *text, size_t length);
static int grow_table(struct line_table *table);
static long *match_lines(const struct diff_file *first, const struct diff_file *second);
static int match_work_allocate(struct match_work *work, long count_a, long count_b, long ids);
static void match_work_release(struct match_work *work);
static void keep_matchable(const long *ids, long count, const long *other_counts, long *kept_ids, long *map, long *kept_count);
static int solve_range(struct lcs_state *state, long a_low, long a_high, long b_low, long b_high);
static int find_middle(const struct lcs_state *state, long a_low, long a_high, long b_low, long b_high, long *split_a, long *split_b);

/*
 * Loads a file as lines.  Writes a diagnostic-free failure: the caller
 * reports errno.
 */
int
diff_load(
	const char *path,
	const char *label,
	struct diff_file *file)
{
	char *grown;
	size_t capacity;
	ssize_t got;
	size_t index;
	long count;
	int descriptor;

	/* Opens the file. */
	memset(file, 0, sizeof(*file));
	file->label = label;
	descriptor = open(path, O_RDONLY);
	if (descriptor < 0)
		return -1;

	/* Reads all of it. */
	capacity = 65536;
	file->data = malloc(capacity);
	if (file->data == NULL) {
		close(descriptor);
		errno = ENOMEM;
		return -1;
	}

	/* Reads until the end of the file. */
	for (;;) {
		/* Grows the buffer when it is full. */
		if (file->size == capacity) {
			capacity *= 2;
			grown = realloc(file->data, capacity);
			if (grown == NULL) {
				close(descriptor);
				errno = ENOMEM;
				return -1;
			}

			/* Uses the larger buffer from now on. */
			file->data = grown;
		}

		/* Reads what fits; the end of the file ends the loop. */
		got = read(descriptor, file->data + file->size, capacity - file->size);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0) {
			close(descriptor);
			return -1;
		}

		/* The end of the file ends the loop. */
		if (got == 0)
			break;
		file->size += (size_t)got;
	}

	/* Closes the file; everything has been read. */
	close(descriptor);

	/* Counts the lines; a last line without a newline counts too. */
	count = 0;
	for (index = 0; index < file->size; index++) {
		if (file->data[index] == '\n')
			count++;
	}

	/* A last line without a newline counts too. */
	if (file->size > 0 && file->data[file->size - 1] != '\n') {
		count++;
		file->missing_newline = 1;
	}

	/* Records where each line starts, and the end. */
	file->starts = malloc(((size_t)count + 1) * sizeof(*file->starts));
	file->ids = malloc(((size_t)count + 1) * sizeof(*file->ids));
	if (file->starts == NULL || file->ids == NULL) {
		errno = ENOMEM;
		return -1;
	}

	/* Records where each line starts. */
	count = 0;
	file->starts[0] = 0;
	for (index = 0; index < file->size; index++) {
		if (file->data[index] == '\n') {
			count++;
			file->starts[count] = index + 1;
		}
	}

	/* A last line without a newline ends at the end of the data. */
	if (file->missing_newline) {
		count++;
		file->starts[count] = file->size;
	}

	/* Succeeded: the file as lines. */
	file->count = count;
	return 0;
}

/* Releases a loaded file. */
void
diff_release(
	struct diff_file *file)
{
	/* Frees what diff_load() allocated. */
	free(file->data);
	free(file->starts);
	free(file->ids);
	memset(file, 0, sizeof(*file));
}

/*
 * Numbers the lines of both files so that equal lines share a number.
 * With fold_blanks (-b), trailing blanks are ignored and every other run
 * of blanks compares as one space.
 */
int
diff_number_lines(
	struct diff_file *first,
	struct diff_file *second,
	int fold_blanks)
{
	struct line_table table;
	size_t index;
	int status;
	int failed;

	/* Starts an empty table. */
	table.size = LINES_TABLE_SIZE;
	table.used = 0;
	table.entries = calloc(table.size, sizeof(*table.entries));
	if (table.entries == NULL)
		return -1;

	/* Numbers both files through the one table. */
	failed = 0;
	status = number_file(&table, first, fold_blanks);
	if (status != 0)
		failed = 1;
	status = number_file(&table, second, fold_blanks);
	if (status != 0)
		failed = 1;

	/* Frees the table; the numbers stay with the files. */
	for (index = 0; index < table.size; index++)
		free(table.entries[index].key);
	free(table.entries);

	/* Reports a lack of memory. */
	if (failed) {
		errno = ENOMEM;
		return -1;
	}

	/* Succeeded: every line has its number. */
	return 0;
}

/*
 * Finds the blocks of lines that differ between two numbered files.  The
 * caller frees the array of changes.
 */
int
diff_compare(
	const struct diff_file *first,
	const struct diff_file *second,
	struct diff_change **changes,
	long *count)
{
	struct diff_change *list;
	long *match_a;
	long a_index;
	long b_index;
	long a_end;
	long b_end;
	long used;

	/* Matches the lines of a longest common subsequence. */
	match_a = match_lines(first, second);
	if (match_a == NULL) {
		errno = ENOMEM;
		return -1;
	}

	/* Room for the most changes there can be. */
	list = malloc(((size_t)first->count + (size_t)second->count + 1) * sizeof(*list));
	if (list == NULL) {
		free(match_a);
		errno = ENOMEM;
		return -1;
	}

	/* Walks both files, turning each run of unmatched lines into a change. */
	used = 0;
	a_index = 0;
	b_index = 0;
	while (a_index < first->count || b_index < second->count) {
		/* A matched pair moves both files on. */
		if (a_index < first->count && match_a[a_index] == b_index) {
			a_index++;
			b_index++;
			continue;
		}

		/* The unmatched lines of the first file up to the next match. */
		a_end = a_index;
		while (a_end < first->count && match_a[a_end] < 0)
			a_end++;

		/* And those of the second file before the line that match pairs with. */
		b_end = second->count;
		if (a_end < first->count)
			b_end = match_a[a_end];

		/* Records the change. */
		list[used].a_start = a_index;
		list[used].a_end = a_end;
		list[used].b_start = b_index;
		list[used].b_end = b_end;
		used++;
		a_index = a_end;
		b_index = b_end;
	}

	/* Succeeded: the changes, in order. */
	free(match_a);
	*changes = list;
	*count = used;
	return 0;
}

/* Gives the length of a line without its newline. */
size_t
diff_line_length(
	const struct diff_file *file,
	long line)
{
	size_t length;

	/* The distance to the next line, less the newline that ends it. */
	length = file->starts[line + 1] - file->starts[line];
	if (length > 0 && file->data[file->starts[line + 1] - 1] == '\n')
		length--;

	/* Reports the length. */
	return length;
}

/* Numbers every line of one file through the table. */
static int
number_file(
	struct line_table *table,
	struct diff_file *file,
	int fold_blanks)
{
	long line;
	size_t length;
	int failed;
	int unterminated;

	/*
	 * Numbers each line.  A last line without a newline differs from the
	 * same text with one.
	 */
	failed = 0;
	for (line = 0; line < file->count; line++) {
		length = diff_line_length(file, line);
		unterminated = 0;
		if (!fold_blanks && file->missing_newline && line == file->count - 1)
			unterminated = 1;
		file->ids[line] = line_id(table, file->data + file->starts[line], length, fold_blanks, unterminated, &failed);
		if (failed)
			return -1;
	}

	/* Succeeded: the file is numbered. */
	return 0;
}

/*
 * Gives the number of a line: the number of an equal line seen before,
 * or a new one.
 */
static long
line_id(
	struct line_table *table,
	const char *text,
	size_t length,
	int fold_blanks,
	int unterminated,
	int *failed)
{
	struct line_entry *entry;
	unsigned long hash;
	size_t slot;
	char *key;
	size_t key_length;
	int status;
	int compare;

	/* The key is the line, or the line with its blanks folded. */
	key = malloc(length + 2);
	if (key == NULL) {
		*failed = 1;
		return -1;
	}

	/* Folds the blanks under -b, or copies the line. */
	if (fold_blanks) {
		key_length = fold_line(text, length, key);
	} else {
		memcpy(key, text, length);
		key_length = length;
	}

	/* Terminates the key. */
	key[key_length] = '\0';

	/* A newline, which no line holds, marks a line that lacks one. */
	if (unterminated) {
		key[key_length] = '\n';
		key_length++;
		key[key_length] = '\0';
	}

	/* Keeps the table at most half full. */
	if (table->used * 2 >= table->size) {
		status = grow_table(table);
		if (status != 0) {
			free(key);
			*failed = 1;
			return -1;
		}
	}

	/* Probes for the key. */
	hash = hash_bytes(key, key_length);
	slot = hash % table->size;
	for (;;) {
		entry = &table->entries[slot];

		/* An empty slot: the line is new and gets the next number. */
		if (entry->key == NULL) {
			entry->key = key;
			entry->length = key_length;
			entry->hash = hash;
			entry->id = (long)table->used;
			table->used++;
			return entry->id;
		}

		/* An equal key: the line shares its number. */
		if (entry->hash == hash && entry->length == key_length) {
			compare = memcmp(entry->key, key, key_length);
			if (compare == 0) {
				free(key);
				return entry->id;
			}
		}

		/* Goes on to the next slot. */
		slot = (slot + 1) % table->size;
	}
}

/*
 * Folds the blanks of a line for -b: trailing blanks are dropped and each
 * other run of spaces and tabs becomes one space.  Returns the length.
 */
static size_t
fold_line(
	const char *text,
	size_t length,
	char *out)
{
	size_t index;
	size_t used;
	int blank;
	int in_run;

	/* Copies the line, one space for each run of blanks. */
	used = 0;
	in_run = 0;
	for (index = 0; index < length; index++) {
		blank = 0;
		if (text[index] == ' ' || text[index] == '\t')
			blank = 1;

		/* A blank starts or continues a run. */
		if (blank) {
			in_run = 1;
			continue;
		}

		/* A run before another character is one space. */
		if (in_run)
			out[used++] = ' ';
		in_run = 0;
		out[used++] = text[index];
	}

	/* A run at the end is dropped. */
	return used;
}

/* Hashes bytes (FNV-1a). */
static unsigned long
hash_bytes(
	const char *text,
	size_t length)
{
	unsigned long hash;
	size_t index;

	/* Mixes in each byte. */
	hash = 2166136261UL;
	for (index = 0; index < length; index++) {
		hash ^= (unsigned char)text[index];
		hash *= 16777619UL;
	}

	/* Reports the hash. */
	return hash;
}

/* Doubles the table, placing every entry again. */
static int
grow_table(
	struct line_table *table)
{
	struct line_entry *entries;
	size_t size;
	size_t index;
	size_t slot;

	/* Allocates the larger table. */
	size = table->size * 2;
	entries = calloc(size, sizeof(*entries));
	if (entries == NULL)
		return -1;

	/* Places each entry in its new slot. */
	for (index = 0; index < table->size; index++) {
		if (table->entries[index].key == NULL)
			continue;
		slot = table->entries[index].hash % size;
		while (entries[slot].key != NULL)
			slot = (slot + 1) % size;
		entries[slot] = table->entries[index];
	}

	/* Uses the larger table from now on. */
	free(table->entries);
	table->entries = entries;
	table->size = size;
	return 0;
}

/*
 * Matches the lines of a longest common subsequence of two files.
 * Returns, for each line of the first file, the line of the second it is
 * matched with or -1; NULL without memory.
 */
static long *
match_lines(
	const struct diff_file *first,
	const struct diff_file *second)
{
	struct lcs_state state;
	struct match_work work;
	long *result;
	long ids;
	long index;
	int status;

	/* The number of distinct lines bounds the ids. */
	ids = 0;
	for (index = 0; index < first->count; index++) {
		if (first->ids[index] >= ids)
			ids = first->ids[index] + 1;
	}

	/* The second file's lines may add ids of their own. */
	for (index = 0; index < second->count; index++) {
		if (second->ids[index] >= ids)
			ids = second->ids[index] + 1;
	}

	/* Allocates the work arrays; a failure frees what was allocated. */
	status = match_work_allocate(&work, first->count, second->count, ids);
	if (status != 0)
		return NULL;

	/* Counts how often each line occurs in each file. */
	for (index = 0; index < first->count; index++)
		work.counts_a[first->ids[index]]++;
	for (index = 0; index < second->count; index++)
		work.counts_b[second->ids[index]]++;

	/* Keeps only the lines the other file also has. */
	keep_matchable(first->ids, first->count, work.counts_b, work.kept_a, work.map_a, &work.kept_a_count);
	keep_matchable(second->ids, second->count, work.counts_a, work.kept_b, work.map_b, &work.kept_b_count);

	/* Solves the kept lines. */
	for (index = 0; index < work.kept_a_count; index++)
		work.match[index] = -1;
	state.a = work.kept_a;
	state.b = work.kept_b;
	state.match_a = work.match;
	status = solve_range(&state, 0, work.kept_a_count, 0, work.kept_b_count);
	if (status != 0) {
		match_work_release(&work);
		return NULL;
	}

	/* Carries the matches back to the lines of the files. */
	result = work.result;
	for (index = 0; index < first->count; index++)
		result[index] = -1;
	for (index = 0; index < work.kept_a_count; index++) {
		if (work.match[index] >= 0)
			result[work.map_a[index]] = work.map_b[work.match[index]];
	}

	/* Succeeded: the matches, which the caller now owns. */
	work.result = NULL;
	match_work_release(&work);
	return result;
}

/* Allocates the arrays of one matching, one at a time. */
static int
match_work_allocate(
	struct match_work *work,
	long count_a,
	long count_b,
	long ids)
{
	/* Nothing is allocated yet. */
	memset(work, 0, sizeof(*work));

	/* The occurrence counts of each line. */
	work->counts_a = calloc((size_t)ids + 1, sizeof(*work->counts_a));
	if (work->counts_a == NULL) {
		match_work_release(work);
		return -1;
	}

	/* The counts of the second file. */
	work->counts_b = calloc((size_t)ids + 1, sizeof(*work->counts_b));
	if (work->counts_b == NULL) {
		match_work_release(work);
		return -1;
	}

	/* The kept lines of each file and where they came from. */
	work->kept_a = malloc(((size_t)count_a + 1) * sizeof(*work->kept_a));
	if (work->kept_a == NULL) {
		match_work_release(work);
		return -1;
	}

	/* The kept lines of the second file. */
	work->kept_b = malloc(((size_t)count_b + 1) * sizeof(*work->kept_b));
	if (work->kept_b == NULL) {
		match_work_release(work);
		return -1;
	}

	/* Where the kept lines of the first file came from. */
	work->map_a = malloc(((size_t)count_a + 1) * sizeof(*work->map_a));
	if (work->map_a == NULL) {
		match_work_release(work);
		return -1;
	}

	/* Where the kept lines of the second file came from. */
	work->map_b = malloc(((size_t)count_b + 1) * sizeof(*work->map_b));
	if (work->map_b == NULL) {
		match_work_release(work);
		return -1;
	}

	/* The matches of the kept lines. */
	work->match = malloc(((size_t)count_a + 1) * sizeof(*work->match));
	if (work->match == NULL) {
		match_work_release(work);
		return -1;
	}

	/* The matches of the whole first file. */
	work->result = malloc(((size_t)count_a + 1) * sizeof(*work->result));
	if (work->result == NULL) {
		match_work_release(work);
		return -1;
	}

	/* Succeeded: every array is allocated. */
	return 0;
}

/* Frees the arrays of one matching. */
static void
match_work_release(
	struct match_work *work)
{
	/* Frees each array; free ignores those never allocated. */
	free(work->counts_a);
	free(work->counts_b);
	free(work->kept_a);
	free(work->kept_b);
	free(work->map_a);
	free(work->map_b);
	free(work->match);
	free(work->result);
	memset(work, 0, sizeof(*work));
}

/*
 * Copies the ids of the lines whose id the other file has at least once,
 * and the original line number of each kept line.
 */
static void
keep_matchable(
	const long *ids,
	long count,
	const long *other_counts,
	long *kept_ids,
	long *map,
	long *kept_count)
{
	long index;
	long used;

	/* Keeps each line the other file has. */
	used = 0;
	for (index = 0; index < count; index++) {
		if (other_counts[ids[index]] == 0)
			continue;
		kept_ids[used] = ids[index];
		map[used] = index;
		used++;
	}

	/* Reports how many were kept. */
	*kept_count = used;
}

/*
 * Matches the lines of a common subsequence of a[a_low, a_high) and
 * b[b_low, b_high): the common start and end, then the two halves around
 * the middle of an optimal path.
 */
static int
solve_range(
	struct lcs_state *state,
	long a_low,
	long a_high,
	long b_low,
	long b_high)
{
	long split_a;
	long split_b;
	int found;
	int status;

	/* Matches the common start. */
	while (a_low < a_high && b_low < b_high && state->a[a_low] == state->b[b_low]) {
		state->match_a[a_low] = b_low;
		a_low++;
		b_low++;
	}

	/* Matches the common end. */
	while (a_low < a_high && b_low < b_high && state->a[a_high - 1] == state->b[b_high - 1]) {
		a_high--;
		b_high--;
		state->match_a[a_high] = b_high;
	}

	/* A range left empty on one side has nothing more in common. */
	if (a_low == a_high || b_low == b_high)
		return 0;

	/* Finds the middle of an optimal path. */
	found = find_middle(state, a_low, a_high, b_low, b_high, &split_a, &split_b);
	if (found < 0)
		return -1;
	if (found == 0)
		return 0;

	/* Solves the two halves. */
	status = solve_range(state, a_low, split_a, b_low, split_b);
	if (status != 0)
		return -1;
	status = solve_range(state, split_a, a_high, split_b, b_high);
	if (status != 0)
		return -1;

	/* Succeeded: the range is matched. */
	return 0;
}

/*
 * Finds a point in the middle of an optimal edit path through a range by
 * extending paths from both ends until they meet.  Returns 1 and the
 * point, 0 when the ranges have nothing in common, or -1 without memory.
 */
static int
find_middle(
	const struct lcs_state *state,
	long a_low,
	long a_high,
	long b_low,
	long b_high,
	long *split_a,
	long *split_b)
{
	const long *a;
	const long *b;
	long *forward;
	long *backward;
	long length_a;
	long length_b;
	long max_d;
	long offset;
	long size;
	long delta;
	long d;
	long k;
	long k_offset;
	long other;
	long x;
	long y;
	long x_back;
	long start_forward;
	long end_forward;
	long start_backward;
	long end_backward;
	long index;
	int odd;

	/* The two ranges and the arrays of furthest reaching paths. */
	a = state->a + a_low;
	b = state->b + b_low;
	length_a = a_high - a_low;
	length_b = b_high - b_low;
	max_d = (length_a + length_b + 1) / 2;
	offset = max_d;
	size = 2 * max_d + 2;
	forward = malloc((size_t)size * sizeof(*forward));
	backward = malloc((size_t)size * sizeof(*backward));
	if (forward == NULL || backward == NULL) {
		free(forward);
		free(backward);
		return -1;
	}

	/* Marks every diagonal as not reached yet. */
	for (index = 0; index < size; index++) {
		forward[index] = -1;
		backward[index] = -1;
	}

	/* The paths start at the two corners. */
	forward[offset + 1] = 0;
	backward[offset + 1] = 0;

	/* An odd difference in length meets on a forward step, an even one backward. */
	delta = length_a - length_b;
	odd = 0;
	if (delta % 2 != 0)
		odd = 1;
	start_forward = 0;
	end_forward = 0;
	start_backward = 0;
	end_backward = 0;

	/* Extends the paths one edit at a time. */
	for (d = 0; d < max_d; d++) {
		/* The forward paths. */
		for (k = -d + start_forward; k <= d - end_forward; k += 2) {
			k_offset = offset + k;
			if (k == -d || (k != d && forward[k_offset - 1] < forward[k_offset + 1]))
				x = forward[k_offset + 1];
			else
				x = forward[k_offset - 1] + 1;
			y = x - k;
			while (x < length_a && y < length_b && a[x] == b[y]) {
				x++;
				y++;
			}

			/* Remembers how far the path on this diagonal reaches. */
			forward[k_offset] = x;

			/* A path off the edge limits later diagonals. */
			if (x > length_a) {
				end_forward += 2;
				continue;
			}

			/* A path below the edge limits the diagonals from the other side. */
			if (y > length_b) {
				start_forward += 2;
				continue;
			}

			/* On an odd delta, a forward path may meet a backward one. */
			if (odd) {
				other = offset + delta - k;
				if (other >= 0 && other < size && backward[other] != -1) {
					x_back = length_a - backward[other];
					if (x >= x_back) {
						*split_a = a_low + x;
						*split_b = b_low + y;
						free(forward);
						free(backward);
						return 1;
					}
				}
			}
		}

		/* The backward paths. */
		for (k = -d + start_backward; k <= d - end_backward; k += 2) {
			k_offset = offset + k;
			if (k == -d || (k != d && backward[k_offset - 1] < backward[k_offset + 1]))
				x = backward[k_offset + 1];
			else
				x = backward[k_offset - 1] + 1;
			y = x - k;
			while (x < length_a && y < length_b && a[length_a - x - 1] == b[length_b - y - 1]) {
				x++;
				y++;
			}

			/* Remembers how far the path on this diagonal reaches. */
			backward[k_offset] = x;

			/* A path off the edge limits later diagonals. */
			if (x > length_a) {
				end_backward += 2;
				continue;
			}

			/* A path below the edge limits the diagonals from the other side. */
			if (y > length_b) {
				start_backward += 2;
				continue;
			}

			/* On an even delta, a backward path may meet a forward one. */
			if (!odd) {
				other = offset + delta - k;
				if (other >= 0 && other < size && forward[other] != -1) {
					x_back = forward[other];
					y = offset + x_back - other;
					if (x_back >= length_a - x) {
						*split_a = a_low + x_back;
						*split_b = b_low + y;
						free(forward);
						free(backward);
						return 1;
					}
				}
			}
		}
	}

	/* The paths never met: nothing is in common. */
	free(forward);
	free(backward);
	return 0;
}
