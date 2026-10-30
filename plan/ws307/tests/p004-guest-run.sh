#!/bin/sh
# Runs on the installed system (as its user tester, through plan/ws307/tests/p004-gtk.sh, in the directory DIR that
# holds p004-hello.c): pkg-config finds GTK 4, the system's own clang builds p004-hello.c with the flags it gives, the
# program runs in the user's desktop session and shows its window ("P004 MAPPED"), and a small C++ program builds
# and runs against libc++.  Each step prints "STEP name ok|fail"; the last line is "P004 RUN DONE".
#   sh p004-guest-run.sh DIR
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
d=${1:?usage: p004-guest-run.sh DIR}
cd "$d" || exit 1
step() { sname=$1; shift; if "$@" > step.txt 2>&1; then echo "STEP $sname ok"; else echo "STEP $sname fail"; cat step.txt; fi; }

echo "PKGCONF $(pkg-config --version 2>&1)"
echo "CLANG $(cc --version 2>&1 | head -1)"
step pkg-config-gtk4 pkg-config --exists --print-errors gtk4
echo "CFLAGS $(pkg-config --cflags gtk4 2>&1)"
echo "LIBS $(pkg-config --libs gtk4 2>&1)"
step build sh -c 'cc -Wall -Wextra -Werror -o p004-hello p004-hello.c $(pkg-config --cflags --libs gtk4)'

# The desktop session of the automatic login is the user's own, at /run/user/<uid>.  Its first login opens Settings'
# Welcome over the whole screen; it is closed so the window is what the picture shows (chosen by the command's name,
# not by its arguments, which this script's own command line would also match).
uid=$(id -u)
for pid in $(ps -A -o pid,comm | awk '$2 == "settings" { print $1 }'); do
	kill "$pid" 2> /dev/null
done
sleep 2
echo "SESSION $(ls /run/user/$uid 2>&1 | tr '\n' ' ')"
: > hello.out
XDG_RUNTIME_DIR=/run/user/$uid WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland GSK_RENDERER=cairo GTK_A11Y=none \
	./p004-hello > hello.out 2>&1
status=$?
sed 's/^/HELLO /' hello.out
step hello-status test $status -eq 0
step hello-mapped grep -q '^P004 MAPPED gtk=4\.' hello.out

# An observation, not judged: GTK reported GTK_A11Y with a broken value.  A small program run with the same variables
# prints what getenv() and environ hold, to see whether the C library gives the value as it was passed.
cat > p004-env.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>

extern char **environ;

int main(void)
{
	const char *value;
	char **entry;

	value = getenv("GTK_A11Y");
	if (value == NULL)
		value = "(null)";
	printf("getenv GTK_A11Y=%s\n", value);
	for (entry = environ; *entry != NULL; entry++)
		printf("environ %s\n", *entry);
	return 0;
}
EOF
if cc -o p004-env p004-env.c > env-build.txt 2>&1; then
	XDG_RUNTIME_DIR=/run/user/$uid WAYLAND_DISPLAY=wayland-0 GDK_BACKEND=wayland GSK_RENDERER=cairo GTK_A11Y=none \
		./p004-env 2>&1 | sed 's/^/ENVTEST /'
else
	sed 's/^/ENVTEST build /' env-build.txt
fi

cat > p004-cxx.cc <<'EOF'
#include <iostream>
#include <string>
#include <vector>

int main()
{
	std::vector<std::string> words{"libc++", "on", "zedBSD"};
	std::string line;
	for (const auto &word : words)
		line += (line.empty() ? "" : " ") + word;
	std::cout << "P004 CXX " << line << std::endl;
	return 0;
}
EOF
step build-cxx clang++ -std=c++17 -Wall -Wextra -Werror -o p004-cxx p004-cxx.cc
./p004-cxx > cxx.out 2>&1
sed 's/^/CXX /' cxx.out
step cxx-run grep -q '^P004 CXX libc++ on zedBSD$' cxx.out
echo "P004 RUN DONE"
