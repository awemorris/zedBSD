/*
 * zedBSD
 * Copyright (C) 2026 Awe Morris
 *
 * SPDX-License-Identifier: Zlib
 */

/*
 * User authentication via /sbin/passkey and isolated headless Wayland
 * session spawning for rdpd.
 */

#include "rdpd.h"
#include "userland/desktop/wayland/rdp-headless.h"

#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static int write_str_fd(int fd, const char *s, size_t len);
static void drop_privileges(uid_t uid, gid_t gid, const char *user, const char *home, const char *shell, const char *runtime_dir);

/* Writes len bytes to fd, retrying on EINTR. */
static int
write_str_fd(
	int fd,
	const char *s,
	size_t len)
{
	size_t off;
	ssize_t n;
	off = 0U;
	while (off < len) {
		n = write(fd, s + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return EIO;
		}

		/* Advances past the written slice. */
		off += (size_t)n;
	}

	/* Succeeded. */
	return 0;
}

static int
rdp_sock_connect(
	void)
{
	struct sockaddr_un addr;
	int fd;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	(void)snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/sessiond-rdp.sock");
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
hex_val(
	char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

int
rdpd_extend_lookup(
	const char *user,
	uint8_t out_nt_hash[16])
{
	char req[96];
	char resp[96];
	ssize_t n;
	int fd;
	size_t i;

	if (user == NULL || user[0] == '\0' || out_nt_hash == NULL)
		return -1;
	fd = rdp_sock_connect();
	if (fd < 0)
		return -1;
	(void)snprintf(req, sizeof(req), "RDP_EXTEND_LOOKUP %s\n", user);
	if (write_str_fd(fd, req, strlen(req)) != 0) {
		close(fd);
		return -1;
	}
	n = read(fd, resp, sizeof(resp) - 1U);
	close(fd);
	if (n <= 0)
		return -1;
	resp[n] = '\0';
	if (strncmp(resp, "OK ", 3U) != 0 || strlen(resp) < 35U)
		return -1;
	for (i = 0U; i < 16U; i++) {
		int hi = hex_val(resp[3U + i * 2U]);
		int lo = hex_val(resp[3U + i * 2U + 1U]);
		if (hi < 0 || lo < 0)
			return -1;
		out_nt_hash[i] = (uint8_t)((hi << 4) | lo);
	}
	memset(resp, 0, sizeof(resp));
	return 0;
}

int
rdpd_extend_attach(
	const char *user,
	uint32_t width,
	uint32_t height,
	int *out_rdp_fd)
{
	char req[128];
	char resp[32];
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr msg;
	struct iovec vec;
	struct cmsghdr *cmsg;
	ssize_t n;
	int fd;

	if (user == NULL || out_rdp_fd == NULL)
		return -1;
	*out_rdp_fd = -1;
	fd = rdp_sock_connect();
	if (fd < 0)
		return -1;
	(void)snprintf(req, sizeof(req), "RDP_EXTEND %s %u %u\n",
		       user, (unsigned)width, (unsigned)height);
	if (write_str_fd(fd, req, strlen(req)) != 0) {
		close(fd);
		return -1;
	}
	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	vec.iov_base = resp;
	vec.iov_len = sizeof(resp) - 1U;
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	msg.msg_controllen = sizeof(control.space);
	n = recvmsg(fd, &msg, 0);
	close(fd);
	if (n <= 0)
		return -1;
	resp[n] = '\0';
	if (strncmp(resp, "OK", 2U) != 0)
		return -1;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(out_rdp_fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}
	return (*out_rdp_fd >= 0) ? 0 : -1;
}

/* Authenticates user/password via /sbin/passkey and returns account info. */
int
rdpd_auth_user(
	const char *user,
	const char *password,
	uid_t *out_uid,
	gid_t *out_gid,
	char *out_home,
	size_t home_cap,
	char *out_shell,
	size_t shell_cap)
{
	struct passwd pwd;
	struct passwd *res;
	char buf[2048];
	int in_pipe[2];
	int out_pipe[2];
	pid_t pid;
	char reply[128];
	ssize_t n;
	int status;
	int rc;
	int d1;
	int d2;
	char *argv[2];
	pid_t wp;
	int exited;
	int exit_code;
	if (user == NULL || user[0] == '\0' || password == NULL)
		return EINVAL;
	res = NULL;
	rc = getpwnam_r(user, &pwd, buf, sizeof(buf), &res);
	if (rc != 0 || res == NULL)
		return EACCES;
	rc = pipe(in_pipe);
	if (rc != 0)
		return errno;
	rc = pipe(out_pipe);
	if (rc != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return errno;
	}

	/* Spawns passkey to verify the password. */
	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return errno;
	}

	/* Child executes passkey check. */
	if (pid == 0) {
		close(in_pipe[1]);
		close(out_pipe[0]);
		d1 = dup2(in_pipe[0], STDIN_FILENO);
		d2 = dup2(out_pipe[1], STDOUT_FILENO);
		if (d1 < 0 || d2 < 0)
			_exit(127);
		close(in_pipe[0]);
		close(out_pipe[1]);
		argv[0] = (char *)RDPD_PASSKEY_PATH;
		argv[1] = NULL;
		execv(RDPD_PASSKEY_PATH, argv);
		_exit(127);
	}

	/* Sends 4-line passkey auth request and waits for exit code. */
	close(in_pipe[0]);
	close(out_pipe[1]);
	(void)write_str_fd(in_pipe[1], "auth\n", 5U);
	(void)write_str_fd(in_pipe[1], user, strlen(user));
	(void)write_str_fd(in_pipe[1], "\npassword\n", 10U);
	(void)write_str_fd(in_pipe[1], password, strlen(password));
	(void)write_str_fd(in_pipe[1], "\n", 1U);
	close(in_pipe[1]);
	memset(reply, 0, sizeof(reply));
	n = read(out_pipe[0], reply, sizeof(reply) - 1U);
	close(out_pipe[0]);
	status = 0;
	for (;;) {
		wp = waitpid(pid, &status, 0);
		if (wp < 0 && errno == EINTR)
			continue;
		break;
	}

	/* Verifies passkey exit status. */
	exited = WIFEXITED(status);
	exit_code = WEXITSTATUS(status);
	if (!exited || exit_code != 0)
		return EACCES;
	(void)n;
	if (out_uid != NULL)
		*out_uid = pwd.pw_uid;
	if (out_gid != NULL)
		*out_gid = pwd.pw_gid;
	if (out_home != NULL && home_cap > 0U)
		(void)snprintf(out_home, home_cap, "%s", pwd.pw_dir);
	if (out_shell != NULL && shell_cap > 0U)
		(void)snprintf(out_shell, shell_cap, "%s", pwd.pw_shell);
	return 0;
}

/* Drops root privileges and prepares session environment variables. */
static void
drop_privileges(
	uid_t uid,
	gid_t gid,
	const char *user,
	const char *home,
	const char *shell,
	const char *runtime_dir)
{
	(void)initgroups(user, gid);
	(void)setgid(gid);
	(void)setuid(uid);
	(void)setenv("HOME", home, 1);
	(void)setenv("USER", user, 1);
	(void)setenv("LOGNAME", user, 1);
	(void)setenv("SHELL", shell, 1);
	(void)setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
}

/* Claims an isolated seat socket from sessiond for /dev/input/event<event_dev>. */
static void
session_claim_seat(
	uint32_t event_dev,
	int *out_owner_fd,
	int *out_seat_fd)
{
	struct sockaddr_un addr;
	char req[32];
	char resp[32];
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr msg;
	struct iovec vec;
	struct cmsghdr *cmsg;
	ssize_t n;
	int fd;

	*out_owner_fd = -1;
	*out_seat_fd = -1;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	(void)snprintf(addr.sun_path, sizeof(addr.sun_path), "/run/sessiond-rdp.sock");
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(fd);
		return;
	}
	(void)snprintf(req, sizeof(req), "RDP_SEAT %u\n", (unsigned)event_dev);
	if (write_str_fd(fd, req, strlen(req)) != 0) {
		close(fd);
		return;
	}
	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	vec.iov_base = resp;
	vec.iov_len = sizeof(resp) - 1U;
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	msg.msg_controllen = sizeof(control.space);
	n = recvmsg(fd, &msg, 0);
	if (n <= 0) {
		close(fd);
		return;
	}
	resp[n] = '\0';
	if (strncmp(resp, "OK", 2U) != 0) {
		close(fd);
		return;
	}
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(out_seat_fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}
	if (*out_seat_fd < 0) {
		close(fd);
		return;
	}
	*out_owner_fd = fd;
}

/* Claims an isolated session control socket from sessiond for admin actions. */
static void
session_claim_control(
	uid_t uid,
	int *out_control_fd)
{
	char req[32];
	char resp[32];
	union {
		struct cmsghdr header;
		char space[CMSG_SPACE(sizeof(int))];
	} control;
	struct msghdr msg;
	struct iovec vec;
	struct cmsghdr *cmsg;
	ssize_t n;
	int fd;

	*out_control_fd = -1;
	fd = rdp_sock_connect();
	if (fd < 0)
		return;
	(void)snprintf(req, sizeof(req), "RDP_CONTROL %lu\n", (unsigned long)uid);
	if (write_str_fd(fd, req, strlen(req)) != 0) {
		close(fd);
		return;
	}
	memset(&msg, 0, sizeof(msg));
	memset(&control, 0, sizeof(control));
	vec.iov_base = resp;
	vec.iov_len = sizeof(resp) - 1U;
	msg.msg_iov = &vec;
	msg.msg_iovlen = 1;
	msg.msg_control = control.space;
	msg.msg_controllen = sizeof(control.space);
	n = recvmsg(fd, &msg, 0);
	close(fd);
	if (n <= 0)
		return;
	resp[n] = '\0';
	if (strncmp(resp, "OK", 2U) != 0)
		return;
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(out_control_fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}
}

/* Launches headless wayland (--headless --rdp-fd=N --seat-fd=4) via sessiond seat isolation. */
int
rdpd_session_start(
	struct rdpd_session *sess,
	uid_t uid,
	gid_t gid,
	const char *user,
	const char *home,
	const char *shell,
	uint32_t width,
	uint32_t height,
	uint32_t event_dev)
{
	int sv[2];
	int seat_fd;
	int control_fd;
	char user_run[64];
	char runtime_dir[96];
	char opt_width[32];
	char opt_height[32];
	char opt_fd[32];
	char opt_sock[160];
	char opt_seat[24];
	char opt_ctrl[24];
	char *comp_argv[16];
	int argc;
	pid_t comp_pid;
	int rc;
	int err;
	memset(sess, 0, sizeof(*sess));
	sess->rdp_fd = -1;
	sess->sessiond_fd = -1;
	sess->width = 1280U;
	if (width != 0U)
		sess->width = width;
	sess->height = 720U;
	if (height != 0U)
		sess->height = height;
	(void)mkdir("/run/user", 0755);
	(void)snprintf(user_run, sizeof(user_run), "/run/user/%lu",
		       (unsigned long)uid);
	(void)mkdir(user_run, 0700);
	(void)chown(user_run, uid, gid);
	(void)snprintf(runtime_dir, sizeof(runtime_dir), "%s/rdp-%ld",
		       user_run, (long)getpid());
	(void)mkdir(runtime_dir, 0700);
	(void)chown(runtime_dir, uid, gid);
	session_claim_seat(event_dev, &sess->sessiond_fd, &seat_fd);
	session_claim_control(uid, &control_fd);
	rc = socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
	if (rc != 0) {
		if (control_fd >= 0)
			close(control_fd);
		if (seat_fd >= 0)
			close(seat_fd);
		if (sess->sessiond_fd >= 0) {
			close(sess->sessiond_fd);
			sess->sessiond_fd = -1;
		}
		return errno;
	}
	(void)snprintf(sess->wayland_socket, sizeof(sess->wayland_socket),
		       "%s/wayland-0", runtime_dir);
	(void)snprintf(opt_width, sizeof(opt_width), "--width=%u", sess->width);
	(void)snprintf(opt_height, sizeof(opt_height), "--height=%u", sess->height);
	(void)snprintf(opt_fd, sizeof(opt_fd), "--rdp-fd=%d", sv[1]);
	(void)snprintf(opt_sock, sizeof(opt_sock), "--socket=%s",
		       sess->wayland_socket);
	(void)snprintf(opt_seat, sizeof(opt_seat), "--seat-fd=%d", 4);
	(void)snprintf(opt_ctrl, sizeof(opt_ctrl), "--control-fd=%d", 3);
	comp_pid = fork();
	if (comp_pid < 0) {
		err = errno;
		close(sv[0]);
		close(sv[1]);
		if (control_fd >= 0)
			close(control_fd);
		if (seat_fd >= 0)
			close(seat_fd);
		if (sess->sessiond_fd >= 0) {
			close(sess->sessiond_fd);
			sess->sessiond_fd = -1;
		}
		return err;
	}

	/* Child drops privileges and execs headless wayland with full desktop look. */
	if (comp_pid == 0) {
		close(sv[0]);
		if (sess->sessiond_fd >= 0)
			close(sess->sessiond_fd);
		if (sv[1] == 3 || sv[1] == 4) {
			int moved = fcntl(sv[1], F_DUPFD, 5);
			if (moved >= 0) {
				close(sv[1]);
				sv[1] = moved;
				(void)snprintf(opt_fd, sizeof(opt_fd), "--rdp-fd=%d", sv[1]);
			}
		}
		(void)fcntl(sv[1], F_SETFD, 0);
		if (control_fd >= 0) {
			if (control_fd == 4 && seat_fd >= 0) {
				int moved = fcntl(control_fd, F_DUPFD, 5);
				if (moved >= 0) {
					close(control_fd);
					control_fd = moved;
				}
			}
			if (control_fd != 3) {
				(void)dup2(control_fd, 3);
				close(control_fd);
			}
			(void)fcntl(3, F_SETFD, 0);
		}
		if (seat_fd >= 0) {
			if (seat_fd != 4) {
				(void)dup2(seat_fd, 4);
				close(seat_fd);
			}
			(void)fcntl(4, F_SETFD, 0);
		}
		drop_privileges(uid, gid, user, home, shell, runtime_dir);
		argc = 0;
		comp_argv[argc++] = (char *)RDPD_WAYLAND_PATH;
		comp_argv[argc++] = (char *)"--headless";
		comp_argv[argc++] = opt_width;
		comp_argv[argc++] = opt_height;
		comp_argv[argc++] = opt_fd;
		comp_argv[argc++] = opt_sock;
		if (control_fd >= 0)
			comp_argv[argc++] = opt_ctrl;
		if (seat_fd >= 0)
			comp_argv[argc++] = opt_seat;
		comp_argv[argc++] = (char *)"--session";
		comp_argv[argc++] = (char *)"--glass";
		if (access("/usr/share/keiland/wallpaper.png", R_OK) == 0)
			comp_argv[argc++] = (char *)"--wallpaper=/usr/share/keiland/wallpaper.png";
		comp_argv[argc] = NULL;
		execv(RDPD_WAYLAND_PATH, comp_argv);
		_exit(127);
	}

	/* Records compositor state without spawning a duplicate compositor. */
	close(sv[1]);
	if (control_fd >= 0)
		close(control_fd);
	if (seat_fd >= 0)
		close(seat_fd);
	sess->compositor_pid = comp_pid;
	sess->rdp_fd = sv[0];
	return 0;
}

/* Sends a RESIZE control message to the headless compositor. */
void
rdpd_session_resize(
	struct rdpd_session *sess,
	uint32_t width,
	uint32_t height)
{
	uint8_t msg[KWL_RDP_CTRL_HDR_SIZE];
	if (sess == NULL || sess->rdp_fd < 0)
		return;
	sess->width = width;
	sess->height = height;
	kwl_rdp_pack_ctrl(msg, KWL_RDP_MSG_RESIZE, width, height);
	(void)write(sess->rdp_fd, msg, sizeof(msg));
}

/* Sends a FORCE_KEY control message to the headless compositor. */
void
rdpd_session_force_key(
	struct rdpd_session *sess)
{
	uint8_t msg[KWL_RDP_CTRL_HDR_SIZE];
	if (sess == NULL || sess->rdp_fd < 0)
		return;
	kwl_rdp_pack_ctrl(msg, KWL_RDP_MSG_FORCE_KEY, 0U, 0U);
	(void)write(sess->rdp_fd, msg, sizeof(msg));
}

/* Terminates the headless compositor and session processes. */
void
rdpd_session_stop(
	struct rdpd_session *sess)
{
	int status;
	pid_t wp;
	if (sess == NULL)
		return;
	if (sess->rdp_fd >= 0) {
		close(sess->rdp_fd);
		sess->rdp_fd = -1;
	}
	if (sess->sessiond_fd >= 0) {
		close(sess->sessiond_fd);
		sess->sessiond_fd = -1;
	}

	/* Stops the session script process. */
	if (sess->session_pid > 0) {
		(void)kill(sess->session_pid, SIGTERM);
		for (;;) {
			wp = waitpid(sess->session_pid, &status, 0);
			if (wp < 0 && errno == EINTR)
				continue;
			break;
		}

		/* Clears the stopped session PID. */
		sess->session_pid = 0;
	}

	/* Stops the headless Wayland compositor process. */
	if (sess->compositor_pid > 0) {
		(void)kill(sess->compositor_pid, SIGTERM);
		for (;;) {
			wp = waitpid(sess->compositor_pid, &status, 0);
			if (wp < 0 && errno == EINTR)
				continue;
			break;
		}

		/* Clears the stopped compositor PID. */
		sess->compositor_pid = 0;
	}
}
