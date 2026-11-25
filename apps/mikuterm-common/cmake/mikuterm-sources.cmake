# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

#
# The source lists of mikuterm that its builds share: the files in apps/mikuterm-common, the
# terminal's and the SSH library's modules that mikuterm uses, and a reader of the tree's
# sources.mk lists.  The build that includes this sets MIKUTERM_ROOT (the tree's top) first.
# Every list is of absolute paths.
#
#   MIKUTERM_COMMON_SOURCES          every build
#   MIKUTERM_COMMON_X11_SOURCES      a build that has X11 forwarding (MIKUTERM_HAVE_X11 = 1)
#   MIKUTERM_COMMON_X11_THREAD_SOURCES  a build whose X server runs in a thread of the same process
#   MIKUTERM_COMMON_PROGRAM_SOURCES  a build that starts programs of the computer's for ProxyCommand
#                                    and GitHub Codespaces (MIKUTERM_HAVE_PROXY_COMMAND, _CODESPACES)
#   MIKUTERM_COMMON_NO_WSL_SOURCES   a build without WSL (MIKUTERM_HAVE_WSL = 0)
#   MIKUTERM_COMMON_NO_X11_SOURCES   a build without X11 forwarding (MIKUTERM_HAVE_X11 = 0)
#   MIKUTERM_TERMINAL_SOURCES        the terminal's core and its settings, for the processor's drawing
#   MIKUTERM_SSH_SOURCES             the SSH library's modules the client engine needs
#   MIKUTERM_VERSION_IN              the template of the version header
#

if(NOT DEFINED MIKUTERM_ROOT)
	message(FATAL_ERROR "set MIKUTERM_ROOT before including mikuterm-sources.cmake")
endif()

set(MIKUTERM_COMMON_DIR ${MIKUTERM_ROOT}/apps/mikuterm-common)

#
# Reads one source list of a sources.mk ("NAME := file file \" and its continuation lines) into
# a CMake list of absolute paths.  The file is read again when it changes.
#
function(mikuterm_read_sources variable file name)
	file(READ ${MIKUTERM_ROOT}/${file} text)
	set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${MIKUTERM_ROOT}/${file})
	string(REGEX REPLACE "\\\\\n" " " text "${text}")
	string(REGEX MATCH "(^|\n)${name}[ \t]*:=([^\n]*)" found "${text}")
	if(NOT found)
		message(FATAL_ERROR "${file} has no ${name}")
	endif()
	string(REGEX MATCHALL "[^ \t]+\\.c" files "${CMAKE_MATCH_2}")
	list(TRANSFORM files PREPEND ${MIKUTERM_ROOT}/)
	set(${variable} ${files} PARENT_SCOPE)
endfunction()

set(MIKUTERM_COMMON_NAMES
	conf settings-file connection-ssh known-hosts ssh-thread forward remote-command launch page sftp-link)
list(TRANSFORM MIKUTERM_COMMON_NAMES PREPEND ${MIKUTERM_COMMON_DIR}/ OUTPUT_VARIABLE MIKUTERM_COMMON_SOURCES)
list(TRANSFORM MIKUTERM_COMMON_SOURCES APPEND .c)

set(MIKUTERM_COMMON_SFTP_SOURCES
	${MIKUTERM_COMMON_DIR}/sftp-worker.c
	${MIKUTERM_COMMON_DIR}/sftp-transfer.c
	${MIKUTERM_COMMON_DIR}/sftp-view.c
	${MIKUTERM_COMMON_DIR}/sftp-view-draw.c
	${MIKUTERM_COMMON_DIR}/sftp-view-input.c)

set(MIKUTERM_COMMON_RDP_SOURCES
	${MIKUTERM_COMMON_DIR}/rdp-hosts.c
	${MIKUTERM_COMMON_DIR}/rdp-h264sw.c
	${MIKUTERM_COMMON_DIR}/rdp-tab.c
	${MIKUTERM_COMMON_DIR}/rdp-thread.c
	${MIKUTERM_COMMON_DIR}/netdisp-proto.c
	${MIKUTERM_COMMON_DIR}/netdisp-tab.c)

set(MIKUTERM_COMMON_MEDIA_SOURCES
	${MIKUTERM_COMMON_DIR}/rdp-media.c)

set(MIKUTERM_COMMON_X11_NAMES x11-link x11-host x11-serve x11-tab x11-noglx x11-touch)
list(TRANSFORM MIKUTERM_COMMON_X11_NAMES PREPEND ${MIKUTERM_COMMON_DIR}/ OUTPUT_VARIABLE MIKUTERM_COMMON_X11_SOURCES)
list(TRANSFORM MIKUTERM_COMMON_X11_SOURCES APPEND .c)

set(MIKUTERM_COMMON_X11_THREAD_SOURCES
	${MIKUTERM_COMMON_DIR}/x11-spawn-thread.c
	${MIKUTERM_COMMON_DIR}/x11-tab-only.c)

set(MIKUTERM_COMMON_PROGRAM_SOURCES
	${MIKUTERM_COMMON_DIR}/proxy-command.c
	${MIKUTERM_COMMON_DIR}/codespace.c)

set(MIKUTERM_COMMON_NO_WSL_SOURCES ${MIKUTERM_COMMON_DIR}/wsl-none.c)
set(MIKUTERM_COMMON_NO_X11_SOURCES ${MIKUTERM_COMMON_DIR}/x11-none.c)

# The terminal's core, its settings' tab, the drawing on the processor and the pacing by the clock.
set(MIKUTERM_TERMINAL_NAMES
	run screen width search keys font render-build render-cpu display-cpu frame-timer
	window menu tabs clipboard primary touch settings-page settings-key settings-hosts)
list(TRANSFORM MIKUTERM_TERMINAL_NAMES PREPEND ${MIKUTERM_ROOT}/userland/desktop/terminal/ OUTPUT_VARIABLE MIKUTERM_TERMINAL_SOURCES)
list(TRANSFORM MIKUTERM_TERMINAL_SOURCES APPEND .c)

# The SSH library's modules the client engine needs, and the engine (not the programs' files).
set(MIKUTERM_SSH_NAMES
	buf packet cipher crypto bcrypt kex transport auth channel key hostfile config common client-engine sftp)
list(TRANSFORM MIKUTERM_SSH_NAMES PREPEND ${MIKUTERM_ROOT}/userland/base/ssh/ssh- OUTPUT_VARIABLE MIKUTERM_SSH_SOURCES)
list(TRANSFORM MIKUTERM_SSH_SOURCES APPEND .c)
list(APPEND MIKUTERM_SSH_SOURCES ${MIKUTERM_ROOT}/userland/base/ssh/sftp-client.c)

set(MIKUTERM_VERSION_IN ${MIKUTERM_COMMON_DIR}/mikuterm-version.h.in)
