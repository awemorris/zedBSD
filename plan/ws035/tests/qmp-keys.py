#!/usr/bin/env python3
"""Types text into a QEMU guest as key presses through one QMP connection.

    qmp-keys.py SOCKET TEXT...

Each character becomes a press and a release of its key on a US layout,
with shift held for the shifted characters; "\\n" is Enter.  A word of the
form <name> presses one QEMU qcode (for example <esc>, <ctrl-c>).
Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
import json
import socket
import sys
import time

PLAIN = {' ': 'spc', '-': 'minus', '=': 'equal', '[': 'bracket_left', ']': 'bracket_right',
         ';': 'semicolon', "'": 'apostrophe', '`': 'grave_accent', '\\': 'backslash', ',': 'comma',
         '.': 'dot', '/': 'slash', '\n': 'ret', '\t': 'tab'}
SHIFTED = {'!': '1', '@': '2', '#': '3', '$': '4', '%': '5', '^': '6', '&': '7', '*': '8', '(': '9',
           ')': '0', '_': 'minus', '+': 'equal', '{': 'bracket_left', '}': 'bracket_right',
           ':': 'semicolon', '"': 'apostrophe', '~': 'grave_accent', '|': 'backslash', '<': 'comma',
           '>': 'dot', '?': 'slash'}


def key_of(character):
    """Returns (qcode, shifted) for one character."""
    if character.isalpha() and character.isascii():
        return character.lower(), character.isupper()
    if character.isdigit():
        return character, False
    if character in PLAIN:
        return PLAIN[character], False
    if character in SHIFTED:
        return SHIFTED[character], True
    raise ValueError(f'no key for {character!r}')


def main():
    connection = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    connection.connect(sys.argv[1])
    stream = connection.makefile('rw')
    stream.readline()

    def call(command, arguments=None):
        request = {'execute': command}
        if arguments is not None:
            request['arguments'] = arguments
        stream.write(json.dumps(request) + '\n')
        stream.flush()
        while True:
            reply = json.loads(stream.readline())
            if 'return' in reply or 'error' in reply:
                return reply

    def key(code, down):
        call('input-send-event', {'events': [{'type': 'key', 'data': {'down': down, 'key': {'type': 'qcode', 'data': code}}}]})
        time.sleep(0.03)

    call('qmp_capabilities')
    for text in sys.argv[2:]:
        if text.startswith('<') and text.endswith('>'):
            names = text[1:-1].split('-')
            modifiers = {'ctrl': 'ctrl', 'shift': 'shift', 'alt': 'alt'}
            held = [modifiers[name] for name in names[:-1]]
            for code in held:
                key(code, True)
            key(names[-1], True)
            key(names[-1], False)
            for code in reversed(held):
                key(code, False)
            continue
        for character in text.encode().decode('unicode_escape'):
            code, shifted = key_of(character)
            if shifted:
                key('shift', True)
            key(code, True)
            key(code, False)
            if shifted:
                key('shift', False)
    return 0


if __name__ == '__main__':
    sys.exit(main())
