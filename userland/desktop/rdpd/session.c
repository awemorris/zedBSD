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
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int
write_str_fd(
	int fd,
	const char *s,
	size_t len)
{
	size_t off;

	off = 0U;
	while (off < len) {
		ssize_t n;

		n = write(fd, s + off, len - off);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return EIO;
		}
		off += (size_t)n;
	}
	return 0;
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

	if (user == NULL || user[0] == '\0' || password == NULL)
		return EINVAL;

	res = NULL;
	if (getpwnam_r(user, &pwd, buf, sizeof(buf), &res) != 0 || res == NULL)
		return EACCES;

	if (pipe(in_pipe) != 0)
		return errno;
	if (pipe(out_pipe) != 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		return errno;
	}

	pid = fork();
	if (pid < 0) {
		close(in_pipe[0]);
		close(in_pipe[1]);
		close(out_pipe[0]);
		close(out_pipe[1]);
		return errno;
	}

	if (pid == 0) {
		char *argv[3];

		close(in_pipe[1]);
		close(out_pipe[0]);
		if (dup2(in_pipe[0], STDIN_FILENO) < 0 ||
		    dup2(out_pipe[1], STDOUT_FILENO) < 0)
			_exit(127);
		close(in_pipe[0]);
		close(out_pipe[1]);
		argv[0] = (char *)RDPD_PASSKEY_PATH;
		argv[1] = (char *)"check";
		argv[2] = NULL;
		execv(RDPD_PASSKEY_PATH, argv);
		_exit(127);
	}

	close(in_pipe[0]);
	close(out_pipe[1]);

	(void)write_str_fd(in_pipe[1], user, strlen(user));
	(void)write_str_fd(in_pipe[1], "\n", 1U);
	(void)write_str_fd(in_pipe[1], password, strlen(password));
	(void)write_str_fd(in_pipe[1], "\n", 1U);
	close(in_pipe[1]);

	memset(reply, 0, sizeof(reply));
	n = read(out_pipe[0], reply, sizeof(reply) - 1U);
	close(out_pipe[0]);

	status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;

	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
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

static void
drop_privileges(
	uid_t uid,
	gid_t gid,
	const char *user,
	const char *home,
	const char *shell)
{
	char runtime_dir[64];

	(void)snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%lu",
		       (unsigned long)uid);
	(void)setgid(gid);
	(void)setuid(uid);
	(void)setenv("HOME", home, 1);
	(void)setenv("USER", user, 1);
	(void)setenv("LOGNAME", user, 1);
	(void)setenv("SHELL", shell, 1);
	(void)setenv("XDG_RUNTIME_DIR", runtime_dir, 1);
}

/* Launches headless wayland (--headless --rdp-fd=N) and /etc/keiland/session. */
int
rdpd_session_start(
	struct rdpd_session *sess,
	uid_t uid,
	gid_t gid,
	const char *user,
	const char *home,
	const char *shell,
	uint32_t width,
	uint32_t height)
{
	int sv[2];
	char runtime_dir[64];
	char opt_width[32];
	char opt_height[32];
	char opt_fd[32];
	char opt_sock[160];
	pid_t comp_pid;
	pid_t sess_pid;

	memset(sess, 0, sizeof(*sess));
	sess->rdp_fd = -1;
	sess->width = width != 0U ? width : 1280U;
	sess->height = height != 0U ? height : 720U;

	(void)mkdir("/run/user", 0755);
	(void)snprintf(runtime_dir, sizeof(runtime_dir), "/run/user/%lu",
		       (unsigned long)uid);
	(void)mkdir(runtime_dir, 0700);
	(void)chown(runtime_dir, uid, gid);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return errno;

	(void)snprintf(sess->wayland_socket, sizeof(sess->wayland_socket),
		       "%s/wayland-rdp-%ld", runtime_dir, (long)getpid());
	(void)snprintf(opt_width, sizeof(opt_width), "--width=%u", sess->width);
	(void)snprintf(opt_height, sizeof(opt_height), "--height=%u", sess->height);
	(void)snprintf(opt_fd, sizeof(opt_fd), "--rdp-fd=%d", sv[1]);
	(void)snprintf(opt_sock, sizeof(opt_sock), "--socket=%s",
		       sess->wayland_socket);

	comp_pid = fork();
	if (comp_pid < 0) {
		int err = errno;
		close(sv[0]);
		close(sv[1]);
		return err;
	}

	if (comp_pid == 0) {
		char *argv[8];

		close(sv[0]);
		(void)fcntl(sv[1], F_SETFD, 0);
		drop_privileges(uid, gid, user, home, shell);
		argv[0] = (char *)RDPD_WAYLAND_PATH;
		argv[1] = (char *)"--headless";
		argv[2] = opt_width;
		argv[3] = opt_height;
		argv[4] = opt_fd;
		argv[5] = opt_sock;
		argv[6] = (char *)"--session";
		argv[7] = NULL;
		execv(RDPD_WAYLAND_PATH, argv);
		_exit(127);
	}

	close(sv[1]);
	sess->compositor_pid = comp_pid;
	sess->rdp_fd = sv[0];

	sess_pid = fork();
	if (sess_pid == 0) {
		char *argv[2];

		close(sv[0]);
		drop_privileges(uid, gid, user, home, shell);
		(void)setenv("WAYLAND_DISPLAY", sess->wayland_socket, 1);
		argv[0] = (char *)RDPD_SESSION_PATH;
		argv[1] = NULL;
		execv(RDPD_SESSION_PATH, argv);
		_exit(127);
	}
	sess->session_pid = sess_pid;
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

	if (sess == NULL)
		return;
	if (sess->rdp_fd >= 0) {
		close(sess->rdp_fd);
		sess->rdp_fd = -1;
	}
	if (sess->session_pid > 0) {
		(void)kill(sess->session_pid, SIGTERM);
		while (waitpid(sess->session_pid, &status, 0) < 0 && errno == EINTR)
			;
		sess->session_pid = 0;
	}
	if (sess->compositor_pid > 0) {
		(void)kill(sess->compositor_pid, SIGTERM);
		while (waitpid(sess->compositor_pid, &status, 0) < 0 && errno == EINTR)
			;
		sess->compositor_pid = 0;
	}
}
