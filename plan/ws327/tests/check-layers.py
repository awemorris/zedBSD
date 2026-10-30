#!/usr/bin/env python3
# The applications and libkeiland outside its platform/ use libkeiland's threads and network, not the system's
# (the checks T1 and N1 of plan/ws327/design.md §1.3, meant to join plan/tools/keiland-os-boundary/check.sh):
#   T1  no pthread_*, thrd_*, mtx_*, cnd_*, call_once, <pthread.h>, <threads.h>
#   N1  no socket, connect, bind, listen, accept, accept4, getaddrinfo, poll, ppoll, select, pselect, epoll_*, kqueue,
#       kevent, send, recv, sendto, recvfrom, setsockopt, shutdown calls, no <sys/socket.h>, <sys/select.h>, <sys/un.h>,
#       <netdb.h>, <poll.h>, <netinet/*>, <arpa/inet.h>
# The sources are check.sh's applications (userland/desktop's folders but the compositor, the system's services, the
# X server, the graphics and compatibility libraries and libkeiland-backend*), less libbrowser and libmedia (libraries
# of their own) and libkeiland/platform/ (the layers themselves).  A call is a name followed by "(" and not preceded
# by a word character, "." or ">" (a member such as ->select( is not one).  Comments and literals are not looked at.
# The allowed lines are in layers-allow.tsv beside this file (each with its reason).
#   python3 -I plan/ws327/tests/check-layers.py [PREFIX...]    (from the worktree's root)
# With PREFIXes only the sources under them are reported (a phase checks its own files).  Prints each finding as
# "check: T1 FAIL path:line: name", a count per check, and "ws327-check-layers: PASS" or "FAIL" (exit 0 or 1).
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ALLOW = os.path.join(HERE, 'layers-allow.tsv')
NOT_APPLICATIONS = {
    'wayland', 'sessiond', 'printd', 'xserver', 'libvulkan', 'libvulkan-compat', 'libGL', 'libegl', 'libglesv2',
    'libwayland', 'libwayland-egl', 'linux-compat', 'freebsd-compat', 'include', 'fonts', 'artwork', 'wallpapers',
    'locale', 'libbrowser', 'libmedia',
}
CALLS = {
    'T1': re.compile(r'(?<![\w.>])(pthread_\w+|thrd_\w+|mtx_\w+|cnd_\w+|call_once)\s*\('),
    'N1': re.compile(r'(?<![\w.>])(socket|connect|bind|listen|accept|accept4|getaddrinfo|poll|ppoll|select|pselect|epoll_\w+|kqueue|kevent|send|recv|sendto|recvfrom|setsockopt|shutdown)\s*\('),
}
TYPES = {
    'T1': re.compile(r'(?<![\w.>])(pthread_\w+_t|thrd_t|mtx_t|cnd_t|once_flag|PTHREAD_\w+)\b'),
}
HEADERS = {
    'T1': re.compile(r'^\s*#\s*include\s*<(pthread\.h|threads\.h)>'),
    'N1': re.compile(r'^\s*#\s*include\s*<(sys/socket\.h|sys/select\.h|sys/un\.h|netdb\.h|poll\.h|netinet/[^>]*|arpa/inet\.h)>'),
}


def load_allowed():
    """The allowed (check, source, name) rows; a row without its reason is a finding of its own."""
    allowed = []
    malformed = []
    with open(ALLOW) as table:
        for line in table:
            if line.startswith('#') or not line.strip():
                continue
            fields = line.rstrip('\n').split('\t')
            if len(fields) < 4 or not fields[3].strip():
                malformed.append(f'{ALLOW}: a row without its four columns: {line.strip()}')
                continue
            allowed.append((fields[0], fields[1], fields[2]))
    return allowed, malformed


def is_allowed(allowed, check, path, name):
    """Whether a row allows a name found by a check in a source."""
    for row_check, source, row_name in allowed:
        if row_check != check:
            continue
        if source.endswith('/'):
            if not path.startswith(source):
                continue
        elif path != source:
            continue
        if row_name == '*' or row_name == name:
            return True
    return False


def strip(text):
    """The code with its comments and literals emptied, its lines kept."""
    code = []
    index = 0
    length = len(text)
    while index < length:
        if text.startswith('/*', index):
            end = text.find('*/', index + 2)
            end = length if end < 0 else end + 2
            code.append('\n' * text.count('\n', index, end))
            index = end
        elif text.startswith('//', index):
            end = text.find('\n', index)
            index = length if end < 0 else end
        elif text[index] in '"\'':
            quote = text[index]
            end = index + 1
            while end < length and text[end] != quote and text[end] != '\n':
                if text[end] == '\\':
                    end += 1
                end += 1
            code.append(quote + quote)
            index = end + 1
        else:
            code.append(text[index])
            index += 1
    return ''.join(code)


def sources():
    """Every C source and header of the applications and of libkeiland outside platform/."""
    found = []
    for name in sorted(os.listdir('userland/desktop')):
        folder = os.path.join('userland/desktop', name)
        if not os.path.isdir(folder) or name in NOT_APPLICATIONS or name.startswith('libkeiland-backend'):
            continue
        for directory, _, files in os.walk(folder):
            if directory.startswith('userland/desktop/libkeiland/platform'):
                continue
            for file in sorted(files):
                if file.endswith(('.c', '.h', '.inc')):
                    found.append(os.path.join(directory, file))
    return found


def main():
    """Checks the sources and reports."""
    if not os.path.isdir('userland/desktop'):
        print('ws327-check-layers: run from the worktree\'s root')
        return 2
    prefixes = sys.argv[1:]
    allowed, malformed = load_allowed()
    found = {'T1': [], 'N1': []}
    for line in malformed:
        found['T1'].append(line)
    for path in sources():
        if prefixes and not any(path.startswith(prefix) for prefix in prefixes):
            continue
        with open(path, errors='replace') as source:
            text = source.read()
        code = strip(text)
        for number, line in enumerate(code.split('\n'), 1):
            for check, pattern in CALLS.items():
                for match in pattern.finditer(line):
                    if not is_allowed(allowed, check, path, match.group(1)):
                        found[check].append(f'{path}:{number}: {match.group(1)}')
            for check, pattern in TYPES.items():
                for match in pattern.finditer(line):
                    if not is_allowed(allowed, check, path, match.group(1)):
                        found[check].append(f'{path}:{number}: {match.group(1)}')
        for number, line in enumerate(text.split('\n'), 1):
            for check, pattern in HEADERS.items():
                match = pattern.match(line)
                if match and not is_allowed(allowed, check, path, match.group(1)):
                    found[check].append(f'{path}:{number}: #include <{match.group(1)}>')
    failed = False
    for check, lines in found.items():
        for line in lines:
            print(f'check: {check} FAIL {line}')
        print(f'check: {check} {len(lines)} found')
        if lines:
            failed = True
    print('ws327-check-layers: FAIL' if failed else 'ws327-check-layers: PASS')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
