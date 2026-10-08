/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * The printers (ws145-p003, plan/ws145/design.md section 4; keiland-backend.h):
 * the same on every system.
 *
 * The settings file holds the printers, a line each, the next number and
 * the default:
 *
 *   # Keiland printers
 *   next-id 3
 *   printer 1 ipp 192.168.1.20 631 /ipp/print Office Printer
 *   printer 2 lpd 192.168.1.30 515 lp 192.168.1.30 (LPD)
 *   default 1
 *
 * A change reads the file again under its lock (the file beside it named
 * .lock), changes what it changes and writes it through a file renamed
 * over it, so that two sessions of the user do not undo each other; a file
 * another session changed is read again when its time changes.
 *
 * The jobs go to keiland-printd (the daemon), started with posix_spawn
 * when there is something to do, its socket on its descriptor 3; it is
 * told JOB (with the document's descriptor), CANCEL, NAME and BYE, and
 * answers a line each (printd.h).  The socket does not block: what cannot
 * be sent waits in a queue.  The jobs not ended are at most 16; the last
 * 16 ended are kept for the lists.
 *
 * The daemon's life (ws177-p023, design §4 and §5.1): a job whose JOB line
 * went but that the daemon had not accepted when it ended is sent again to
 * a new daemon, twice at most; one accepted fails ("daemon"), one being
 * cancelled is cancelled.  A daemon that ends three times in ten seconds
 * (not after BYE) is not started again for a minute, nor one that said
 * FATAL; the jobs meanwhile fail.  A daemon that breaks the protocol (a
 * descriptor sent back, a line too long) is shut for writing and given ten
 * seconds to end before its socket is closed.  The documents' descriptors
 * belong to their jobs; a line waiting only borrows its job's.
 */

#include "userland/desktop/libkeiland-backend/keiland-backend.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <dirent.h>
#include <time.h>
#include <unistd.h>

/* The longest line of the daemon's protocol, and the most lines waiting to be sent. */
#define PRINT_LINE_MAX		1024U
#define PRINT_QUEUE_MAX		64U

/* The changes to the settings file waiting for the writer, and those made and not yet taken. */
#define PRINT_CHANGES_MAX	16U

/* The most jobs not ended, the results waiting, and the names asked. */
#define PRINT_ACTIVE_MAX	16U
#define PRINT_RESULTS_MAX	32U
#define PRINT_NAMES_MAX		16U

/* The default ports and paths. */
#define PRINT_IPP_PORT		631U
#define PRINT_LPD_PORT		515U
#define PRINT_IPP_PATH		"/ipp/print"
#define PRINT_LPD_QUEUE		"lp"

/* The environment's variables handed to the daemon at most. */
#define PRINT_ENVIRONMENT_MAX	64U

/*
 * The daemon's ends counted (not after BYE), the time they are counted in
 * and the rest after them; the times a job is sent again; and how long a
 * daemon shut for writing is given to end (seconds).
 */
#define PRINT_ENDS_MAX		3U
#define PRINT_ENDS_SECONDS	10
#define PRINT_REST_SECONDS	60
#define PRINT_RESENDS_MAX	2U
#define PRINT_SHUT_SECONDS	10

/* The environment, for the daemon. */
extern char **environ;

/*
 * A line waiting to be sent: its bytes, the descriptor that goes with it
 * (-1 for none; the job's, borrowed), the job it is, and whether any of
 * it went (a line begun is sent to its end, never taken out).
 */
struct print_line {
	char text[PRINT_LINE_MAX];
	size_t length;
	int fd;
	uint32_t job;
	int begun;
};

/*
 * A job and what the backend holds for it: its document until the daemon
 * took it, whether its JOB line went (its first byte), whether the daemon
 * accepted it, whether a CANCEL went for it, and how many times it was
 * sent again to a new daemon.
 */
struct print_job {
	struct kl_backend_print_job job;
	int fd;
	int sent;
	int accepted;
	int cancelling;
	unsigned resends;
	uint64_t order;
};

/* An answer waiting to be taken. */
struct print_result {
	uint32_t request;
	int error;
	unsigned saved;
};

/* A name asked of the daemon for a printer added. */
struct print_name {
	uint32_t seq;
	uint32_t printer;
};

/*
 * The printers as the settings file holds them: the printers and the
 * default, the next number, and the file's time when read.
 */
struct print_table {
	struct kl_backend_printer printers[KL_BACKEND_PRINTERS_MAX];
	size_t count;
	uint32_t next_id;
	struct timespec time;
};

/* The kinds of a change to the settings file. */
enum print_change_kind {
	PRINT_CHANGE_ADD,
	PRINT_CHANGE_REMOVE,
	PRINT_CHANGE_DEFAULT,
	PRINT_CHANGE_PATH,
	PRINT_CHANGE_NAMED
};

/*
 * A change to the settings file, made by the writer thread (ws177-p024):
 * what is asked (its request, 0 for the backend's own, the printer and the
 * fields), then what came of it (the errno value, whether the file was
 * written, the printer's number, and the table as the file holds it after).
 */
struct print_change {
	enum print_change_kind kind;
	uint32_t request;
	uint32_t printer;
	unsigned protocol;
	char host[KL_BACKEND_PRINTER_HOST_MAX];
	unsigned port;
	char path[KL_BACKEND_PRINTER_PATH_MAX];
	char name[KL_BACKEND_PRINTER_NAME_MAX];
	int error;
	unsigned saved;
	struct print_table table;
};

/*
 * The printers: the settings file and the table read from it, the jobs,
 * the answers, the daemon (its pid, socket, spool, the bytes of a line,
 * the commands sent), the lines waiting, the names asked, and the next
 * numbers.
 *
 * The writer thread (started at the first change) makes the changes to
 * the settings file: it takes the file's lock, reads it again, changes it
 * and writes it, so that a lock another session holds never stops the
 * compositor's thread.  lock guards the changes waiting (pending) and
 * made (made) and stop; the table is the compositor's thread's, replaced
 * by a made change's.
 */
struct kl_backend_print {
	char config[512];
	char runtime[512];
	char program[512];
	struct print_table table;
	pthread_t writer;
	int writer_started;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	struct print_change *pending[PRINT_CHANGES_MAX];
	size_t pending_count;
	struct print_change *made[PRINT_CHANGES_MAX];
	size_t made_count;
	int stop;
	struct print_job jobs[KL_BACKEND_PRINT_JOBS_MAX];
	size_t job_count;
	struct print_result results[PRINT_RESULTS_MAX];
	size_t result_count;
	pid_t daemon;
	int socket;
	char spool[512];
	char input[PRINT_LINE_MAX];
	size_t input_length;
	unsigned long commands;
	struct print_line queue[PRINT_QUEUE_MAX];
	size_t queue_count;
	struct print_name names[PRINT_NAMES_MAX];
	size_t name_count;
	uint32_t next_request;
	uint32_t next_job;
	uint32_t next_seq;
	uint64_t next_order;
	unsigned changed;

	/*
	 * The daemon's life: the times its last ends were counted (monotonic
	 * seconds, oldest first), until when it is not started, whether BYE
	 * went to the daemon running, and when a daemon shut for writing is
	 * given up (0 for none).
	 */
	time_t ends[PRINT_ENDS_MAX];
	size_t end_count;
	time_t rest_until;
	int bye_sent;
	time_t shut_until;
};

static int print_load(const char *config, struct print_table *table);
static int print_save(const char *config, struct print_table *table);
static int print_lock(const char *config);
static void print_unlock(int fd);
static struct kl_backend_printer *print_printer(struct print_table *table, uint32_t id);
static struct print_job *print_find_job(struct kl_backend_print *print, uint32_t job);
static struct print_job *print_new_job(struct kl_backend_print *print);
static void print_end_job(struct kl_backend_print *print, struct print_job *job, unsigned state, const char *detail);
static void print_result(struct kl_backend_print *print, uint32_t request, int error, unsigned saved);
static int print_start(struct kl_backend_print *print);
static void print_stopped(struct kl_backend_print *print);
static void print_send(struct kl_backend_print *print, int fd, uint32_t job, const char *format, ...) __attribute__((format(printf, 4, 5)));
static void print_flush(struct kl_backend_print *print);
static void print_read(struct kl_backend_print *print);
static void print_line(struct kl_backend_print *print, char *line);
static void print_job_state(struct kl_backend_print *print, uint32_t number, const char *state, const char *detail);
static void print_named(struct kl_backend_print *print, uint32_t seq, char *rest);
static void print_remove_spool(const char *dir);
static int print_change(struct kl_backend_print *print, struct print_change *change);
static void *print_writer(void *argument);
static void print_apply(struct print_change *change);
static void print_made(struct kl_backend_print *print);
static void print_ask_name(struct kl_backend_print *print, const struct kl_backend_printer *printer);
static int print_send_job(struct kl_backend_print *print, struct print_job *job);
static void print_resend(struct kl_backend_print *print);
static void print_shut(struct kl_backend_print *print);
static time_t print_now(void);
static int print_host_ok(const char *host);
static void print_copy(char *to, size_t size, const char *from);

/*
 * Starts the printers from the settings file.
 */
struct kl_backend_print *
kl_backend_print_open(
	const char *config_path,
	const char *runtime_dir,
	const char *program)
{
	struct kl_backend_print *print;
	const char *runtime;

	/* The state, no daemon yet. */
	print = calloc(1, sizeof(*print));
	if (print == NULL)
		return NULL;
	print->socket = -1;
	print->daemon = -1;
	print->table.next_id = 1;
	print->next_request = 1;
	print->next_job = 1;
	print->next_seq = 1;
	print_copy(print->config, sizeof(print->config), config_path);
	print_copy(print->program, sizeof(print->program), program);
	runtime = runtime_dir;
	if (runtime == NULL)
		runtime = getenv("XDG_RUNTIME_DIR");
	if (runtime != NULL)
		print_copy(print->runtime, sizeof(print->runtime), runtime);

	/* The writer's lock and wake (the thread starts at the first change). */
	(void)pthread_mutex_init(&print->lock, NULL);
	(void)pthread_cond_init(&print->wake, NULL);

	/* The printers kept. */
	(void)print_load(print->config, &print->table);
	return print;
}

/*
 * Stops the printers: the daemon's socket shut (it ends and removes its
 * spool), the documents held closed.
 */
void
kl_backend_print_close(
	struct kl_backend_print *print)
{
	size_t index;

	/* Nothing open. */
	if (print == NULL)
		return;

	/* The documents (the lines waiting only borrow them). */
	for (index = 0; index < print->job_count; index++) {
		if (print->jobs[index].fd >= 0)
			(void)close(print->jobs[index].fd);
	}

	/* The writer: the changes waiting made, then it stops. */
	if (print->writer_started) {
		(void)pthread_mutex_lock(&print->lock);

		print->stop = 1;
		(void)pthread_cond_signal(&print->wake);

		(void)pthread_mutex_unlock(&print->lock);
		(void)pthread_join(print->writer, NULL);
	}

	/* The changes made and not taken. */
	for (index = 0; index < print->made_count; index++)
		free(print->made[index]);
	(void)pthread_cond_destroy(&print->wake);
	(void)pthread_mutex_destroy(&print->lock);

	/* The daemon's socket: its end ends the daemon. */
	if (print->socket >= 0)
		(void)close(print->socket);
	free(print);
}

/*
 * Tells whether the daemon's program can be run.
 */
int
kl_backend_print_can(
	const struct kl_backend_print *print)
{
	int status;

	/* The program, executable. */
	if (print == NULL)
		return 0;
	status = access(print->program, X_OK);
	return status == 0;
}

/*
 * Reads what the daemon said, sends what waits, and reads the settings
 * file again when another session changed it.
 */
int
kl_backend_print_update(
	struct kl_backend_print *print,
	unsigned *changed)
{
	struct stat status;
	time_t now;
	int error;

	/* The daemon's lines, and the lines waiting for it. */
	*changed = 0;
	if (print == NULL)
		return 0;
	print_made(print);
	if (print->socket >= 0)
		print_read(print);
	if (print->socket >= 0)
		print_flush(print);

	/* A daemon shut for writing that did not end in its time: its socket closed. */
	now = print_now();
	if (print->socket >= 0 && print->shut_until != 0 && now >= print->shut_until)
		print_stopped(print);

	/* The settings file changed by another session. */
	error = stat(print->config, &status);
	if (error == 0 && (status.st_mtim.tv_sec != print->table.time.tv_sec || status.st_mtim.tv_nsec != print->table.time.tv_nsec)) {
		(void)print_load(print->config, &print->table);
		print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;
	}

	/* What changed since the last update. */
	*changed = print->changed;
	print->changed = 0;
	return 0;
}

/*
 * Copies the printers.
 */
size_t
kl_backend_print_printers(
	const struct kl_backend_print *print,
	struct kl_backend_printer *list,
	size_t capacity)
{
	size_t count;

	/* As many as fit. */
	count = print->table.count;
	if (count > capacity)
		count = capacity;
	memcpy(list, print->table.printers, count * sizeof(list[0]));
	return count;
}

/*
 * Copies the jobs, oldest first.
 */
size_t
kl_backend_print_jobs(
	const struct kl_backend_print *print,
	struct kl_backend_print_job *list,
	size_t capacity)
{
	size_t count;
	size_t index;

	/* As many as fit. */
	count = 0;
	for (index = 0; index < print->job_count && count < capacity; index++) {
		list[count] = print->jobs[index].job;
		count++;
	}

	/* The count copied. */
	return count;
}

/*
 * Adds a printer (its path or queue "" for the protocol's usual one): the
 * writer thread adds it to the settings file; an IPP printer's name is
 * asked of the daemon once it is there.
 */
int
kl_backend_print_add(
	struct kl_backend_print *print,
	unsigned protocol,
	const char *host,
	unsigned port,
	const char *path,
	uint32_t *request)
{
	struct print_change *change;
	const char *space;
	size_t path_length;
	int host_ok;
	int error;

	/* A protocol, a host, a port. */
	if (protocol != KL_BACKEND_PRINTER_IPP && protocol != KL_BACKEND_PRINTER_LPD)
		return EINVAL;
	host_ok = print_host_ok(host);
	if (!host_ok || port == 0U || port > 65535U)
		return EINVAL;
	if (path != NULL) {
		path_length = strlen(path);
		space = strchr(path, ' ');
		if (path_length >= KL_BACKEND_PRINTER_PATH_MAX || space != NULL)
			return EINVAL;
	}

	/* The change. */
	change = calloc(1, sizeof(*change));
	if (change == NULL)
		return ENOMEM;
	change->kind = PRINT_CHANGE_ADD;
	change->protocol = protocol;
	print_copy(change->host, sizeof(change->host), host);
	change->port = port;
	print_copy(change->path, sizeof(change->path), path);

	/* Its request, answered when the writer made it. */
	*request = print->next_request;
	print->next_request++;
	change->request = *request;
	error = print_change(print, change);
	if (error != 0)
		print_result(print, *request, error, 0);

	/* Asked: the answer comes as a result. */
	return 0;
}

/*
 * Removes a printer: the writer thread takes it out of the settings file;
 * its jobs not ended are cancelled once it is out.
 */
int
kl_backend_print_remove(
	struct kl_backend_print *print,
	uint32_t printer,
	uint32_t *request)
{
	struct print_change *change;
	int error;

	/* The change. */
	change = calloc(1, sizeof(*change));
	if (change == NULL)
		return ENOMEM;
	change->kind = PRINT_CHANGE_REMOVE;
	change->printer = printer;

	/* Its request, answered when the writer made it. */
	*request = print->next_request;
	print->next_request++;
	change->request = *request;
	error = print_change(print, change);
	if (error != 0)
		print_result(print, *request, error, 0);

	/* Asked: the answer comes as a result. */
	return 0;
}

/*
 * Makes a printer the default: the writer thread writes it in the
 * settings file.
 */
int
kl_backend_print_set_default(
	struct kl_backend_print *print,
	uint32_t printer,
	uint32_t *request)
{
	struct print_change *change;
	int error;

	/* The change. */
	change = calloc(1, sizeof(*change));
	if (change == NULL)
		return ENOMEM;
	change->kind = PRINT_CHANGE_DEFAULT;
	change->printer = printer;

	/* Its request, answered when the writer made it. */
	*request = print->next_request;
	print->next_request++;
	change->request = *request;
	error = print_change(print, change);
	if (error != 0)
		print_result(print, *request, error, 0);

	/* Asked: the answer comes as a result. */
	return 0;
}

/*
 * Prints a document (the descriptor is the backend's from here): a job
 * queued and its JOB line sent to the daemon (started when it is not
 * running).  printer 0 is the default.
 */
int
kl_backend_print_submit(
	struct kl_backend_print *print,
	uint32_t printer,
	const char *title,
	int fd,
	uint32_t *request,
	uint32_t *job)
{
	struct kl_backend_printer *found;
	struct print_job *made;
	size_t active;
	size_t index;
	int error;

	/* The printer: the one named, or the default. */
	*request = print->next_request;
	print->next_request++;
	*job = 0;
	found = NULL;
	for (index = 0; index < print->table.count; index++) {
		if ((printer == 0U && print->table.printers[index].is_default) || print->table.printers[index].id == printer)
			found = &print->table.printers[index];
	}

	/* No such printer: refused. */
	if (found == NULL) {
		(void)close(fd);
		print_result(print, *request, EINVAL, 1);
		return 0;
	}

	/* Not more jobs than are held at once. */
	active = 0;
	for (index = 0; index < print->job_count; index++) {
		if (print->jobs[index].job.state < KL_BACKEND_PRINT_DONE)
			active++;
	}

	/* Too many jobs held: busy. */
	if (active >= PRINT_ACTIVE_MAX) {
		(void)close(fd);
		print_result(print, *request, EBUSY, 1);
		return 0;
	}

	/* The job, queued with its document. */
	made = print_new_job(print);
	made->job.printer = found->id;
	made->job.state = KL_BACKEND_PRINT_QUEUED;
	print_copy(made->job.title, sizeof(made->job.title), title);
	made->fd = fd;
	*job = made->job.job;
	print_result(print, *request, 0, 1);
	print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;

	/* To the daemon, started when it is not running; one that cannot start fails the job. */
	error = print_send_job(print, made);
	if (error != 0)
		print_end_job(print, made, KL_BACKEND_PRINT_FAILED, "daemon");

	/* Asked: the job's state follows. */
	return 0;
}

/*
 * Cancels a job: one whose line has not gone is ended at once; the others
 * are asked of the daemon, which tells how they ended.
 */
int
kl_backend_print_cancel(
	struct kl_backend_print *print,
	uint32_t job,
	uint32_t *request)
{
	struct print_job *found;
	size_t index;

	/* The job, not ended. */
	*request = print->next_request;
	print->next_request++;
	found = print_find_job(print, job);
	if (found == NULL || found->job.state >= KL_BACKEND_PRINT_DONE) {
		print_result(print, *request, EINVAL, 1);
		return 0;
	}

	/*
	 * Its JOB line not begun (or none, the daemon not running): taken out
	 * of the queue, the job cancelled; the daemon never knew it.
	 */
	if (!found->sent) {
		for (index = 0; index < print->queue_count; index++) {
			if (print->queue[index].job != job || print->queue[index].begun)
				continue;
			memmove(&print->queue[index], &print->queue[index + 1U],
			    (print->queue_count - index - 1U) * sizeof(print->queue[0]));
			print->queue_count--;
			break;
		}

		/* Cancelled, its document closed. */
		print_end_job(print, found, KL_BACKEND_PRINT_CANCELLED, "");
		print_result(print, *request, 0, 1);
		return 0;
	}

	/* Asked of the daemon, which tells how it ended (always a CANCEL, even before ACCEPTED). */
	found->cancelling = 1;
	print_send(print, -1, 0, "CANCEL %lu", (unsigned long)job);
	print_result(print, *request, 0, 1);
	return 0;
}

/*
 * Takes the oldest answer.
 */
int
kl_backend_print_take_result(
	struct kl_backend_print *print,
	uint32_t *request,
	int *error,
	unsigned *saved)
{
	/* None. */
	if (print->result_count == 0U)
		return 0;

	/* The first, the rest moved up. */
	*request = print->results[0].request;
	*error = print->results[0].error;
	*saved = print->results[0].saved;
	memmove(&print->results[0], &print->results[1], (print->result_count - 1U) * sizeof(print->results[0]));
	print->result_count--;
	return 1;
}

/* Reads the settings file into a table (none: no printers).  Returns 0 or an errno value. */
static int
print_load(
	const char *config,
	struct print_table *table)
{
	struct kl_backend_printer *printer;
	struct stat status;
	unsigned long id;
	unsigned long port;
	char line[512];
	char protocol[8];
	char host[KL_BACKEND_PRINTER_HOST_MAX];
	char path[KL_BACKEND_PRINTER_PATH_MAX];
	FILE *file;
	char *read;
	int consumed;
	int fields;
	int error;
	int lpd;
	size_t index;

	/* The file, and its time. */
	table->count = 0;
	file = fopen(config, "r");
	if (file == NULL)
		return errno;
	error = fstat(fileno(file), &status);
	if (error == 0)
		table->time = status.st_mtim;

	/* Each line; one that is not understood is passed over. */
	for (;;) {
		read = fgets(line, sizeof(line), file);
		if (read == NULL)
			break;
		line[strcspn(line, "\r\n")] = '\0';

		/* The next number. */
		fields = sscanf(line, "next-id %lu", &id);
		if (fields == 1) {
			if (id > table->next_id)
				table->next_id = (uint32_t)id;
			continue;
		}

		/* The default. */
		fields = sscanf(line, "default %lu", &id);
		if (fields == 1) {
			for (index = 0; index < table->count; index++)
				table->printers[index].is_default = table->printers[index].id == (uint32_t)id;
			continue;
		}

		/* A printer's line. */
		consumed = 0;
		fields = sscanf(line, "printer %lu %7s %63s %lu %63s %n", &id, protocol, host, &port, path, &consumed);
		if (fields != 5 || consumed == 0 || table->count == KL_BACKEND_PRINTERS_MAX || id == 0UL)
			continue;

		/* A printer. */
		printer = &table->printers[table->count];
		memset(printer, 0, sizeof(*printer));
		printer->id = (uint32_t)id;
		lpd = strcmp(protocol, "lpd");
		printer->protocol = KL_BACKEND_PRINTER_IPP;
		if (lpd == 0)
			printer->protocol = KL_BACKEND_PRINTER_LPD;
		print_copy(printer->host, sizeof(printer->host), host);
		printer->port = (unsigned)port;
		print_copy(printer->path, sizeof(printer->path), path);
		print_copy(printer->name, sizeof(printer->name), line + consumed);
		if (printer->id >= table->next_id)
			table->next_id = printer->id + 1U;
		table->count++;
	}

	/* Read. */
	(void)fclose(file);
	return 0;
}

/* Writes a table into the settings file through a file renamed over it.  Returns 0 or an errno value. */
static int
print_save(
	const char *config,
	struct print_table *table)
{
	struct stat status;
	char temporary[600];
	char directory[512];
	const char *protocol;
	char *slash;
	FILE *file;
	size_t index;
	int flushed;
	int failed;
	int closed;
	int error;

	/* The folder, made when it is not there. */
	print_copy(directory, sizeof(directory), config);
	slash = strrchr(directory, '/');
	if (slash != NULL) {
		*slash = '\0';
		(void)mkdir(directory, 0700);
	}

	/* The new file. */
	(void)snprintf(temporary, sizeof(temporary), "%s.new", config);
	file = fopen(temporary, "w");
	if (file == NULL)
		return errno;
	fprintf(file, "# Keiland printers\nnext-id %lu\n", (unsigned long)table->next_id);
	for (index = 0; index < table->count; index++) {
		protocol = "ipp";
		if (table->printers[index].protocol == KL_BACKEND_PRINTER_LPD)
			protocol = "lpd";
		fprintf(file, "printer %lu %s %s %u %s %s\n", (unsigned long)table->printers[index].id, protocol, table->printers[index].host,
		    table->printers[index].port, table->printers[index].path, table->printers[index].name);
	}

	/* The default's line. */
	for (index = 0; index < table->count; index++) {
		if (table->printers[index].is_default)
			fprintf(file, "default %lu\n", (unsigned long)table->printers[index].id);
	}

	/* Written out and closed. */
	flushed = fflush(file);
	failed = ferror(file);
	closed = fclose(file);
	if (flushed != 0 || failed || closed != 0) {
		(void)unlink(temporary);
		return EIO;
	}

	/* In place. */
	error = rename(temporary, config);
	if (error != 0) {
		(void)unlink(temporary);
		return errno;
	}

	/* Its time kept (no reading again for this session's own change). */
	error = stat(config, &status);
	if (error == 0)
		table->time = status.st_mtim;
	return 0;
}

/* Takes the settings file's lock (the file beside it); the descriptor, or -1. */
static int
print_lock(
	const char *config)
{
	char path[600];
	int fd;

	/* The lock file, held. */
	(void)snprintf(path, sizeof(path), "%s.lock", config);
	fd = open(path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (fd < 0)
		return -1;
	(void)flock(fd, LOCK_EX);
	return fd;
}

/* Lets the settings file's lock go. */
static void
print_unlock(
	int fd)
{
	/* Closed: the lock goes with it. */
	if (fd >= 0)
		(void)close(fd);
}

/* Finds a printer by its number. */
static struct kl_backend_printer *
print_printer(
	struct print_table *table,
	uint32_t id)
{
	size_t index;

	/* Each printer. */
	for (index = 0; index < table->count; index++) {
		if (table->printers[index].id == id)
			return &table->printers[index];
	}

	/* Not found. */
	return NULL;
}

/* Finds a job by its number. */
static struct print_job *
print_find_job(
	struct kl_backend_print *print,
	uint32_t job)
{
	size_t index;

	/* Each job. */
	for (index = 0; index < print->job_count; index++) {
		if (print->jobs[index].job.job == job)
			return &print->jobs[index];
	}

	/* Not found. */
	return NULL;
}

/* Makes a job: a free slot, or the oldest ended job's (the table is never full of jobs not ended). */
static struct print_job *
print_new_job(
	struct kl_backend_print *print)
{
	struct print_job *job;
	size_t oldest;
	size_t index;

	/* A free slot. */
	if (print->job_count < KL_BACKEND_PRINT_JOBS_MAX) {
		job = &print->jobs[print->job_count];
		print->job_count++;
	} else {
		/* The oldest ended job's, moved out so that the list stays oldest first. */
		oldest = KL_BACKEND_PRINT_JOBS_MAX;
		for (index = 0; index < print->job_count; index++) {
			if (print->jobs[index].job.state >= KL_BACKEND_PRINT_DONE) {
				oldest = index;
				break;
			}
		}

		/* None ended: the oldest of all. */
		if (oldest == KL_BACKEND_PRINT_JOBS_MAX)
			oldest = 0;
		memmove(&print->jobs[oldest], &print->jobs[oldest + 1U], (print->job_count - oldest - 1U) * sizeof(print->jobs[0]));
		job = &print->jobs[print->job_count - 1U];
	}

	/* Its number. */
	memset(job, 0, sizeof(*job));
	job->fd = -1;
	job->job.job = print->next_job;
	print->next_job++;
	job->order = print->next_order;
	print->next_order++;
	return job;
}

/* Ends a job in a state: its document closed. */
static void
print_end_job(
	struct kl_backend_print *print,
	struct print_job *job,
	unsigned state,
	const char *detail)
{
	/* The document. */
	if (job->fd >= 0)
		(void)close(job->fd);
	job->fd = -1;

	/* The state. */
	job->job.state = state;
	print_copy(job->job.detail, sizeof(job->job.detail), detail);
	print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;
}

/* Keeps an answer (the oldest dropped when they are too many). */
static void
print_result(
	struct kl_backend_print *print,
	uint32_t request,
	int error,
	unsigned saved)
{
	/* Room. */
	if (print->result_count == PRINT_RESULTS_MAX) {
		memmove(&print->results[0], &print->results[1], (PRINT_RESULTS_MAX - 1U) * sizeof(print->results[0]));
		print->result_count--;
	}

	/* The answer. */
	print->results[print->result_count].request = request;
	print->results[print->result_count].error = error;
	print->results[print->result_count].saved = saved;
	print->result_count++;
	print->changed |= KL_BACKEND_PRINT_CHANGED_RESULT;
}

/*
 * Starts the daemon when it is not running: a socket pair, its end on the
 * daemon's descriptor 3, the runtime directory in its environment.
 * Returns 0 or an errno value.
 */
static int
print_start(
	struct kl_backend_print *print)
{
	posix_spawn_file_actions_t actions;
	posix_spawnattr_t attributes;
	sigset_t defaults;
	sigset_t mask;
	char *arguments[2];
	char *environment[PRINT_ENVIRONMENT_MAX];
	char runtime[600];
	size_t count;
	size_t index;
	time_t now;
	pid_t pid;
	int pair[2];
	int child;
	int status;
	int same;

	/* Running already. */
	if (print->socket >= 0)
		return 0;
	if (print->runtime[0] == '\0')
		return ENOENT;

	/* Resting after it ended too often or said FATAL: not started for now. */
	now = print_now();
	if (print->rest_until != 0 && now < print->rest_until)
		return EAGAIN;

	/* The rest is over. */
	print->rest_until = 0;

	/* The socket pair, the backend's end not blocking. */
	status = socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair);
	if (status != 0)
		return errno;
	(void)fcntl(pair[0], F_SETFL, O_NONBLOCK);
	child = pair[1];
	if (child == 3) {
		child = fcntl(pair[1], F_DUPFD_CLOEXEC, 4);
		(void)close(pair[1]);
		if (child < 0) {
			(void)close(pair[0]);
			return EMFILE;
		}
	}

	/* The environment, the runtime directory the backend's. */
	count = 0;
	(void)snprintf(runtime, sizeof(runtime), "XDG_RUNTIME_DIR=%s", print->runtime);
	environment[count] = runtime;
	count++;
	for (index = 0; environ != NULL && environ[index] != NULL && count + 1U < PRINT_ENVIRONMENT_MAX; index++) {
		same = strncmp(environ[index], "XDG_RUNTIME_DIR=", 16U);
		if (same == 0)
			continue;
		environment[count] = environ[index];
		count++;
	}

	/* The list's end. */
	environment[count] = NULL;

	/* The daemon: its descriptor 3, its signals as they start, no mask. */
	(void)posix_spawn_file_actions_init(&actions);
	(void)posix_spawn_file_actions_adddup2(&actions, child, 3);
	(void)posix_spawnattr_init(&attributes);
	(void)sigemptyset(&defaults);
	(void)sigaddset(&defaults, SIGPIPE);
	(void)sigaddset(&defaults, SIGCHLD);
	(void)sigaddset(&defaults, SIGINT);
	(void)sigaddset(&defaults, SIGTERM);
	(void)sigaddset(&defaults, SIGHUP);
	(void)sigemptyset(&mask);
	(void)posix_spawnattr_setsigdefault(&attributes, &defaults);
	(void)posix_spawnattr_setsigmask(&attributes, &mask);
	(void)posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
	arguments[0] = print->program;
	arguments[1] = NULL;
	status = posix_spawn(&pid, print->program, &actions, &attributes, arguments, environment);
	(void)posix_spawn_file_actions_destroy(&actions);
	(void)posix_spawnattr_destroy(&attributes);
	(void)close(child);
	if (status != 0) {
		(void)close(pair[0]);
		return status;
	}

	/* Running: nothing said to it yet. */
	print->daemon = pid;
	print->socket = pair[0];
	print->commands = 0;
	print->input_length = 0;
	print->spool[0] = '\0';
	print->bye_sent = 0;
	print->shut_until = 0;
	return 0;
}

/*
 * The daemon ended (its socket's end, or given up after it was shut): its
 * lines waiting go, its spool is removed, and its jobs not ended go on as
 * the life's table says: one whose JOB went but was not accepted is sent
 * again to a new daemon (twice at most), one being cancelled is
 * cancelled, the others fail.  An end not after BYE is counted: the third
 * in ten seconds rests the daemon for a minute.
 */
static void
print_stopped(
	struct kl_backend_print *print)
{
	struct print_job *job;
	time_t now;
	size_t index;

	/* The socket. */
	(void)close(print->socket);
	print->socket = -1;
	print->daemon = -1;
	print->shut_until = 0;

	/* None waits (the lines only borrowed their jobs' documents), and no name is asked. */
	print->queue_count = 0;
	print->name_count = 0;

	/* An end the backend did not ask for, counted; the third in its time rests the daemon. */
	now = print_now();
	if (!print->bye_sent) {
		if (print->end_count == PRINT_ENDS_MAX) {
			memmove(&print->ends[0], &print->ends[1], (PRINT_ENDS_MAX - 1U) * sizeof(print->ends[0]));
			print->end_count--;
		}

		/* This end, and the rest when it is the third in its time. */
		print->ends[print->end_count] = now;
		print->end_count++;
		if (print->end_count == PRINT_ENDS_MAX && now - print->ends[0] <= PRINT_ENDS_SECONDS)
			print->rest_until = now + PRINT_REST_SECONDS;
	}

	/* The next daemon has had no BYE. */
	print->bye_sent = 0;

	/* The jobs not ended, each as the table says. */
	for (index = 0; index < print->job_count; index++) {
		job = &print->jobs[index];
		if (job->job.state >= KL_BACKEND_PRINT_DONE)
			continue;

		/* Being cancelled: cancelled. */
		if (job->cancelling) {
			print_end_job(print, job, KL_BACKEND_PRINT_CANCELLED, "");
			continue;
		}

		/* Not accepted, its document still held: sent again, twice at most. */
		if (!job->accepted && job->fd >= 0 && job->resends < PRINT_RESENDS_MAX) {
			if (job->sent)
				job->resends++;
			job->sent = 0;
			continue;
		}

		/* Accepted, or sent too often: failed. */
		print_end_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon");
	}

	/* What it left in its spool. */
	if (print->spool[0] != '\0')
		print_remove_spool(print->spool);
	print->spool[0] = '\0';

	/* The jobs to send again, to a new daemon. */
	print_resend(print);
}

/*
 * Sends a job's JOB line with its document to the daemon, started when it
 * is not running.  Returns 0, or an errno value when it cannot start or
 * the job's printer is gone.
 */
static int
print_send_job(
	struct kl_backend_print *print,
	struct print_job *job)
{
	struct kl_backend_printer *printer;
	const char *protocol;
	int error;

	/* The job's printer, still there. */
	printer = print_printer(&print->table, job->job.printer);
	if (printer == NULL)
		return ENOENT;

	/* The daemon. */
	error = print_start(print);
	if (error != 0)
		return error;

	/* The JOB line, with the document beside it (borrowed by the line). */
	protocol = "lpd";
	if (printer->protocol == KL_BACKEND_PRINTER_IPP)
		protocol = "ipp";
	print_send(print, job->fd, job->job.job, "JOB %lu %s %s %u %s %s", (unsigned long)job->job.job, protocol, printer->host,
	    printer->port, printer->path, job->job.title);

	/* Succeeded: the line waits or went. */
	return 0;
}

/* Sends again the jobs not ended whose JOB line has not gone, oldest first; those that cannot go fail. */
static void
print_resend(
	struct kl_backend_print *print)
{
	struct print_job *job;
	size_t index;
	int error;

	/* Each job waiting for a daemon. */
	for (index = 0; index < print->job_count; index++) {
		job = &print->jobs[index];
		if (job->job.state >= KL_BACKEND_PRINT_DONE || job->sent || job->fd < 0)
			continue;

		/* Its line again; no daemon, no job. */
		error = print_send_job(print, job);
		if (error != 0)
			print_end_job(print, job, KL_BACKEND_PRINT_FAILED, "daemon");
	}
}

/*
 * Shuts a daemon that broke the protocol for writing: nothing more is
 * sent, and it is given PRINT_SHUT_SECONDS to end before its socket is
 * closed (so that the old and a new daemon never send the same job).
 */
static void
print_shut(
	struct kl_backend_print *print)
{
	/* Once. */
	if (print->shut_until != 0)
		return;

	/* Shut, and its time given. */
	(void)shutdown(print->socket, SHUT_WR);
	print->shut_until = print_now() + PRINT_SHUT_SECONDS;
}

/* The monotonic clock's seconds (never 0, so that 0 can mean none). */
static time_t
print_now(void)
{
	struct timespec now;

	/* The clock. */
	(void)clock_gettime(CLOCK_MONOTONIC, &now);

	/* Succeeded: the seconds, one past them so that none is 0. */
	return now.tv_sec + 1;
}

/* Queues a line for the daemon (with a descriptor to pass, -1 for none, and the job it is for) and sends what it can. */
static void
print_send(
	struct kl_backend_print *print,
	int fd,
	uint32_t job,
	const char *format,
	...)
{
	struct print_line *line;
	va_list arguments;
	int length;

	/* A full queue, or a daemon shut for writing: the line is lost (a job's document stays its job's). */
	if (print->queue_count == PRINT_QUEUE_MAX || print->shut_until != 0)
		return;

	/* The line. */
	line = &print->queue[print->queue_count];
	va_start(arguments, format);
	length = vsnprintf(line->text, sizeof(line->text) - 1U, format, arguments);
	va_end(arguments);
	if (length < 0)
		length = 0;
	if ((size_t)length > sizeof(line->text) - 2U)
		length = (int)(sizeof(line->text) - 2U);
	line->text[length] = '\n';
	line->length = (size_t)length + 1U;
	line->fd = fd;
	line->job = job;
	line->begun = 0;
	print->queue_count++;
	print->commands++;

	/* Sent now when the socket takes it. */
	print_flush(print);
}

/*
 * Sends the lines waiting, each whole (its descriptor with it); a socket
 * that does not take one stops the sending until the next update.
 */
static void
print_flush(
	struct kl_backend_print *print)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct print_line *line;
	struct print_job *job;
	struct msghdr message;
	struct cmsghdr *rights;
	struct iovec vector;
	ssize_t sent;

	/* Each line, in order. */
	while (print->queue_count > 0U && print->socket >= 0) {
		line = &print->queue[0];
		memset(&message, 0, sizeof(message));
		vector.iov_base = line->text;
		vector.iov_len = line->length;
		message.msg_iov = &vector;
		message.msg_iovlen = 1;

		/* Its descriptor, with its first byte. */
		if (line->fd >= 0) {
			memset(&control, 0, sizeof(control));
			message.msg_control = control.space;
			message.msg_controllen = sizeof(control.space);
			rights = CMSG_FIRSTHDR(&message);
			rights->cmsg_level = SOL_SOCKET;
			rights->cmsg_type = SCM_RIGHTS;
			rights->cmsg_len = CMSG_LEN(sizeof(int));
			memcpy(CMSG_DATA(rights), &line->fd, sizeof(int));
		}

		/* Sent as far as the socket takes it. */
		sent = sendmsg(print->socket, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (sent < 0)
			return;

		/* The descriptor went with the first byte: the job's line went. */
		if (line->fd >= 0 && line->job != 0U) {
			job = print_find_job(print, line->job);
			if (job != NULL)
				job->sent = 1;
		}

		/* Its descriptor is not sent again, and the line is begun. */
		line->fd = -1;
		line->begun = 1;

		/* Part of it: the rest waits. */
		if ((size_t)sent < line->length) {
			memmove(line->text, line->text + sent, line->length - (size_t)sent);
			line->length -= (size_t)sent;
			return;
		}

		/* Whole: the next. */
		memmove(&print->queue[0], &print->queue[1], (print->queue_count - 1U) * sizeof(print->queue[0]));
		print->queue_count--;
	}
}

/*
 * Reads the daemon's lines; its end stops it.  A daemon that sends a
 * descriptor back, whose control data was cut, or whose line is too long
 * broke the protocol: it is shut (print_shut) and what it says after is
 * not taken.
 */
static void
print_read(
	struct kl_backend_print *print)
{
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int) * 4U)];
	} control;
	struct cmsghdr *rights;
	struct msghdr message;
	struct iovec vector;
	char bytes[PRINT_LINE_MAX];
	const int *descriptors;
	size_t count;
	size_t taken;
	ssize_t got;
	ssize_t index;
	int broken;

	/* What there is. */
	for (;;) {
		memset(&message, 0, sizeof(message));
		vector.iov_base = bytes;
		vector.iov_len = sizeof(bytes);
		message.msg_iov = &vector;
		message.msg_iovlen = 1;
		message.msg_control = control.space;
		message.msg_controllen = sizeof(control.space);
		got = recvmsg(print->socket, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (got < 0 && errno == EINTR)
			continue;
		if (got < 0)
			return;
		if (got == 0) {
			print_stopped(print);
			return;
		}

		/* A descriptor sent back is closed: the daemon broke the protocol. */
		broken = (message.msg_flags & MSG_CTRUNC) != 0;
		for (rights = CMSG_FIRSTHDR(&message); rights != NULL; rights = CMSG_NXTHDR(&message, rights)) {
			if (rights->cmsg_level != SOL_SOCKET || rights->cmsg_type != SCM_RIGHTS)
				continue;
			count = (rights->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			descriptors = (const int *)(const void *)CMSG_DATA(rights);
			for (taken = 0; taken < count; taken++)
				(void)close(descriptors[taken]);
			broken = 1;
		}

		/* Broken: shut, and nothing it says is taken. */
		if (broken)
			print_shut(print);
		if (print->shut_until != 0)
			continue;

		/* Line by line. */
		for (index = 0; index < got; index++) {
			if (print->input_length == sizeof(print->input) - 1U) {
				/* A line too long: the protocol broken. */
				print->input_length = 0;
				print_shut(print);
				break;
			}

			/* A byte of the line. */
			if (bytes[index] != '\n') {
				print->input[print->input_length] = bytes[index];
				print->input_length++;
				continue;
			}

			/* A whole line. */
			print->input[print->input_length] = '\0';
			print->input_length = 0;
			print_line(print, print->input);
			if (print->socket < 0)
				return;
		}
	}
}

/* Carries out one of the daemon's lines. */
static void
print_line(
	struct kl_backend_print *print,
	char *line)
{
	struct print_change *change;
	struct print_job *job;
	unsigned long number;
	unsigned long count;
	char word[32];
	char detail[32];
	char path[KL_BACKEND_PRINTER_PATH_MAX];
	int consumed;
	int fields;
	int same;

	/* Where its spool is. */
	same = strncmp(line, "SPOOL ", 6U);
	if (same == 0) {
		print_copy(print->spool, sizeof(print->spool), line + 6);
		return;
	}

	/* A job taken into the spool: its document is the daemon's copy from here. */
	fields = sscanf(line, "ACCEPTED %lu", &number);
	if (fields == 1) {
		job = print_find_job(print, (uint32_t)number);
		if (job != NULL) {
			job->accepted = 1;
			if (job->fd >= 0)
				(void)close(job->fd);
			job->fd = -1;
		}

		/* Done with this line. */
		return;
	}

	/* Refused, with its word when it has one; a job being cancelled is cancelled. */
	detail[0] = '\0';
	fields = sscanf(line, "REJECTED %lu %31s", &number, detail);
	if (fields >= 1) {
		job = print_find_job(print, (uint32_t)number);
		if (job != NULL && job->job.state < KL_BACKEND_PRINT_DONE && job->cancelling)
			print_end_job(print, job, KL_BACKEND_PRINT_CANCELLED, "");
		else if (job != NULL && job->job.state < KL_BACKEND_PRINT_DONE)
			print_end_job(print, job, KL_BACKEND_PRINT_FAILED, detail);
		return;
	}

	/* The daemon cannot run (its runtime directory): not started again for a while. */
	same = strncmp(line, "FATAL", 5U);
	if (same == 0) {
		print->rest_until = print_now() + PRINT_REST_SECONDS;
		return;
	}

	/* A job's state. */
	detail[0] = '\0';
	fields = sscanf(line, "STATE %lu %31s %31s", &number, word, detail);
	if (fields >= 2) {
		print_job_state(print, (uint32_t)number, word, detail);
		return;
	}

	/* The path an IPP printer answered at, kept in the file by the writer. */
	fields = sscanf(line, "PATH %lu %63s", &number, path);
	if (fields == 2) {
		job = print_find_job(print, (uint32_t)number);
		if (job == NULL)
			return;
		change = calloc(1, sizeof(*change));
		if (change == NULL)
			return;
		change->kind = PRINT_CHANGE_PATH;
		change->printer = job->job.printer;
		print_copy(change->path, sizeof(change->path), path);
		(void)print_change(print, change);
		return;
	}

	/* A printer's name. */
	consumed = 0;
	fields = sscanf(line, "NAMED %lu%n", &number, &consumed);
	if (fields == 1) {
		print_named(print, (uint32_t)number, line + consumed);
		return;
	}

	/* Nothing to do: ended when the daemon read every command and nothing waits. */
	fields = sscanf(line, "IDLE %lu", &count);
	if (fields == 1) {
		if (count == print->commands && print->queue_count == 0U) {
			print_send(print, -1, 0, "BYE %lu", count);
			print->commands--;
			print->bye_sent = 1;
		}

		/* Done with this line. */
		return;
	}
}

/* Moves a job to the state the daemon told. */
static void
print_job_state(
	struct kl_backend_print *print,
	uint32_t number,
	const char *state,
	const char *detail)
{
	struct print_job *job;
	int same;

	/* A job not ended. */
	job = print_find_job(print, number);
	if (job == NULL || job->job.state >= KL_BACKEND_PRINT_DONE)
		return;

	/* By the word. */
	print_copy(job->job.detail, sizeof(job->job.detail), detail);
	same = strcmp(state, "sending");
	if (same == 0) {
		job->job.state = KL_BACKEND_PRINT_SENDING;
		print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;
		return;
	}

	/* Waiting. */
	same = strcmp(state, "waiting");
	if (same == 0) {
		job->job.state = KL_BACKEND_PRINT_WAITING;
		print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;
		return;
	}

	/* Done. */
	same = strcmp(state, "done");
	if (same == 0) {
		print_end_job(print, job, KL_BACKEND_PRINT_DONE, detail);
		return;
	}

	/* Cancelled. */
	same = strcmp(state, "cancelled");
	if (same == 0) {
		print_end_job(print, job, KL_BACKEND_PRINT_CANCELLED, detail);
		return;
	}

	/* Any other word: failed. */
	print_end_job(print, job, KL_BACKEND_PRINT_FAILED, detail);
}

/* Keeps a printer's name and path the daemon found (NAMED <seq> <path> <name>, or NAMED <seq> for none), by the writer. */
static void
print_named(
	struct kl_backend_print *print,
	uint32_t seq,
	char *rest)
{
	struct print_change *change;
	uint32_t id;
	size_t index;
	char *name;

	/* The printer asked about. */
	id = 0;
	for (index = 0; index < print->name_count; index++) {
		if (print->names[index].seq != seq)
			continue;
		id = print->names[index].printer;
		memmove(&print->names[index], &print->names[index + 1U], (print->name_count - index - 1U) * sizeof(print->names[0]));
		print->name_count--;
		break;
	}

	/* None asked, or no name found. */
	if (id == 0U || rest[0] != ' ')
		return;

	/* The path, then the name. */
	rest++;
	name = strchr(rest, ' ');
	if (name == NULL)
		return;
	*name = '\0';
	name++;

	/* Kept in the file by the writer. */
	change = calloc(1, sizeof(*change));
	if (change == NULL)
		return;
	change->kind = PRINT_CHANGE_NAMED;
	change->printer = id;
	print_copy(change->path, sizeof(change->path), rest);
	print_copy(change->name, sizeof(change->name), name);
	(void)print_change(print, change);
}

/*
 * Hands a change to the writer thread (started at the first).  The change
 * is the writer's from here.  Returns 0, EBUSY when too many wait, or the
 * thread's errno value (the change is then freed).
 */
static int
print_change(
	struct kl_backend_print *print,
	struct print_change *change)
{
	int error;

	/* The writer, started once. */
	if (!print->writer_started) {
		error = pthread_create(&print->writer, NULL, print_writer, print);
		if (error != 0) {
			free(change);
			return error;
		}

		/* Started, joined at the close. */
		print->writer_started = 1;
	}

	/* Queued for it; too many waiting is busy. */
	(void)pthread_mutex_lock(&print->lock);

	error = 0;
	if (print->pending_count == PRINT_CHANGES_MAX) {
		error = EBUSY;
	} else {
		print->pending[print->pending_count] = change;
		print->pending_count++;
		(void)pthread_cond_signal(&print->wake);
	}

	(void)pthread_mutex_unlock(&print->lock);

	/* Too many waiting: this one goes. */
	if (error != 0) {
		free(change);
		return error;
	}

	/* Succeeded: the writer has it. */
	return 0;
}

/*
 * The writer thread: each change in its turn under the settings file's
 * lock (read again, changed, written), handed back as made, until the
 * backend closes and nothing waits.
 */
static void *
print_writer(
	void *argument)
{
	struct kl_backend_print *print;
	struct print_change *change;
	int written;
	int lock;

	/* Until asked to stop with nothing left. */
	print = argument;
	for (;;) {
		/* The next change, waited for. */
		(void)pthread_mutex_lock(&print->lock);

		while (print->pending_count == 0U && !print->stop)
			(void)pthread_cond_wait(&print->wake, &print->lock);
		change = NULL;
		if (print->pending_count > 0U) {
			change = print->pending[0];
			memmove(&print->pending[0], &print->pending[1], (print->pending_count - 1U) * sizeof(print->pending[0]));
			print->pending_count--;
		}

		(void)pthread_mutex_unlock(&print->lock);

		/* Nothing left and asked to stop. */
		if (change == NULL)
			break;

		/* The file read again under its lock, changed and written. */
		lock = print_lock(print->config);
		change->table.next_id = 1;
		(void)print_load(print->config, &change->table);
		print_apply(change);
		if (change->error == 0) {
			written = print_save(print->config, &change->table);
			change->saved = written == 0;
		}

		/* The lock let go. */
		print_unlock(lock);

		/* Handed back; a full list of made changes drops the oldest. */
		(void)pthread_mutex_lock(&print->lock);

		if (print->made_count == PRINT_CHANGES_MAX) {
			free(print->made[0]);
			memmove(&print->made[0], &print->made[1], (PRINT_CHANGES_MAX - 1U) * sizeof(print->made[0]));
			print->made_count--;
		}

		/* This one, the newest. */
		print->made[print->made_count] = change;
		print->made_count++;

		(void)pthread_mutex_unlock(&print->lock);
	}

	/* Stopped. */
	return NULL;
}

/* Makes a change in its table (the file as it was read); its errno value in change->error. */
static void
print_apply(
	struct print_change *change)
{
	struct print_table *table;
	struct kl_backend_printer *printer;
	struct kl_backend_printer *found;
	size_t index;
	int same_host;
	int was_default;

	/* By its kind. */
	table = &change->table;
	change->error = 0;
	switch (change->kind) {
	case PRINT_CHANGE_ADD:
		/* Room, and not the same printer twice. */
		if (table->count == KL_BACKEND_PRINTERS_MAX) {
			change->error = EBUSY;
			return;
		}

		/* The same protocol, host and port is the same printer. */
		for (index = 0; index < table->count; index++) {
			same_host = strcmp(table->printers[index].host, change->host);
			if (same_host == 0 && table->printers[index].port == change->port && table->printers[index].protocol == change->protocol) {
				change->error = EINVAL;
				return;
			}
		}

		/* The printer, numbered from the file's next number, the first one the default. */
		printer = &table->printers[table->count];
		memset(printer, 0, sizeof(*printer));
		printer->id = table->next_id;
		table->next_id++;
		printer->protocol = change->protocol;
		print_copy(printer->host, sizeof(printer->host), change->host);
		printer->port = change->port;
		if (change->path[0] != '\0')
			print_copy(printer->path, sizeof(printer->path), change->path);
		else if (change->protocol == KL_BACKEND_PRINTER_IPP)
			print_copy(printer->path, sizeof(printer->path), PRINT_IPP_PATH);
		else
			print_copy(printer->path, sizeof(printer->path), PRINT_LPD_QUEUE);
		if (change->protocol == KL_BACKEND_PRINTER_IPP)
			(void)snprintf(printer->name, sizeof(printer->name), "%.60s (IPP)", change->host);
		else
			(void)snprintf(printer->name, sizeof(printer->name), "%.60s (LPD)", change->host);
		printer->is_default = table->count == 0U;
		table->count++;
		change->printer = printer->id;
		return;
	case PRINT_CHANGE_REMOVE:
		/* The printer. */
		found = print_printer(table, change->printer);
		if (found == NULL) {
			change->error = EINVAL;
			return;
		}

		/* Taken out; the default goes to the smallest number left. */
		was_default = found->is_default;
		index = (size_t)(found - table->printers);
		memmove(&table->printers[index], &table->printers[index + 1U], (table->count - index - 1U) * sizeof(table->printers[0]));
		table->count--;
		if (was_default && table->count > 0U) {
			found = &table->printers[0];
			for (index = 1; index < table->count; index++) {
				if (table->printers[index].id < found->id)
					found = &table->printers[index];
			}

			/* It is the default. */
			found->is_default = 1;
		}

		/* Taken out. */
		return;
	case PRINT_CHANGE_DEFAULT:
		/* The printer, it alone the default. */
		found = print_printer(table, change->printer);
		if (found == NULL) {
			change->error = EINVAL;
			return;
		}

		/* The others are not. */
		for (index = 0; index < table->count; index++)
			table->printers[index].is_default = 0;
		found->is_default = 1;
		return;
	case PRINT_CHANGE_PATH:
	case PRINT_CHANGE_NAMED:
		/* The printer's path (and name), when it is still there. */
		found = print_printer(table, change->printer);
		if (found == NULL) {
			change->error = ENOENT;
			return;
		}

		/* What the daemon found. */
		print_copy(found->path, sizeof(found->path), change->path);
		if (change->kind == PRINT_CHANGE_NAMED)
			print_copy(found->name, sizeof(found->name), change->name);
		return;
	default:
		change->error = EINVAL;
		return;
	}
}

/*
 * Takes the changes the writer made: the table as the file holds it, the
 * answers, an IPP printer's name asked, a printer's jobs cancelled when it
 * went.
 */
static void
print_made(
	struct kl_backend_print *print)
{
	struct print_change *change;
	struct kl_backend_printer *printer;
	uint32_t ignored;
	size_t index;

	/* Each change made, oldest first. */
	for (;;) {
		(void)pthread_mutex_lock(&print->lock);

		change = NULL;
		if (print->made_count > 0U) {
			change = print->made[0];
			memmove(&print->made[0], &print->made[1], (print->made_count - 1U) * sizeof(print->made[0]));
			print->made_count--;
		}

		(void)pthread_mutex_unlock(&print->lock);

		/* None left. */
		if (change == NULL)
			return;

		/* The table as the file holds it now, and its answer. */
		print->table = change->table;
		print->changed |= KL_BACKEND_PRINT_CHANGED_LIST;
		if (change->request != 0U)
			print_result(print, change->request, change->error, change->saved);

		/* An IPP printer added: its name asked of the daemon. */
		if (change->kind == PRINT_CHANGE_ADD && change->error == 0 && change->protocol == KL_BACKEND_PRINTER_IPP) {
			printer = print_printer(&print->table, change->printer);
			if (printer != NULL)
				print_ask_name(print, printer);
		}

		/* A printer removed: its jobs not ended, cancelled. */
		if (change->kind == PRINT_CHANGE_REMOVE && change->error == 0) {
			for (index = 0; index < print->job_count; index++) {
				if (print->jobs[index].job.printer == change->printer && print->jobs[index].job.state < KL_BACKEND_PRINT_DONE)
					(void)kl_backend_print_cancel(print, print->jobs[index].job.job, &ignored);
			}
		}

		/* Taken. */
		free(change);
	}
}

/* Asks the daemon (started for it) an IPP printer's name and path. */
static void
print_ask_name(
	struct kl_backend_print *print,
	const struct kl_backend_printer *printer)
{
	int error;

	/* Not more names asked than are kept. */
	if (print->name_count == PRINT_NAMES_MAX)
		return;

	/* The daemon, and the question. */
	error = print_start(print);
	if (error != 0)
		return;
	print->names[print->name_count].seq = print->next_seq;
	print->names[print->name_count].printer = printer->id;
	print->name_count++;
	print_send(print, -1, 0, "NAME %lu %s %u", (unsigned long)print->next_seq, printer->host, printer->port);
	print->next_seq++;
}

/* Removes a dead daemon's spool: its files, the directory and its lock file. */
static void
print_remove_spool(
	const char *dir)
{
	struct dirent *entry;
	char path[1024];
	DIR *opened;

	/* Each file. */
	opened = opendir(dir);
	if (opened != NULL) {
		for (;;) {
			entry = readdir(opened);
			if (entry == NULL)
				break;
			if (entry->d_name[0] == '.')
				continue;
			(void)snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);
			(void)unlink(path);
		}

		/* Read through. */
		(void)closedir(opened);
	}

	/* The directory and its lock. */
	(void)rmdir(dir);
	(void)snprintf(path, sizeof(path), "%s.lock", dir);
	(void)unlink(path);
}

/* Tells whether a host is 1 to 63 letters, digits, dots and hyphens. */
static int
print_host_ok(
	const char *host)
{
	size_t length;
	size_t index;
	char c;

	/* Its length. */
	if (host == NULL)
		return 0;
	length = strlen(host);
	if (length == 0U || length >= KL_BACKEND_PRINTER_HOST_MAX)
		return 0;

	/* Each character. */
	for (index = 0; index < length; index++) {
		c = host[index];
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-'))
			return 0;
	}

	/* Every character is one of them. */
	return 1;
}

/* Copies a text into a field, cut to fit. */
static void
print_copy(
	char *to,
	size_t size,
	const char *from)
{
	/* As much as fits (nothing for none). */
	if (from == NULL)
		from = "";
	(void)snprintf(to, size, "%s", from);
}
