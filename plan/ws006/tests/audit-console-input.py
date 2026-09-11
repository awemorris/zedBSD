#!/usr/bin/env python3
"""IN-T50: reject retired console-event interfaces in live source/headers."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
retired = re.compile(r'\b(?:ZEDBSD_CONSOLE_(?:POLL_EVENT|READ_EVENT|KEY_STATE|GET_INPUT_MODE|SET_INPUT_MODE|DRAIN_INPUT|EVENT_\w+|INPUT_\w+|KEY_\w+)|console_input_event|console_input_mode|console_key_state|console_event|drv_console_input_(?:poll|read)_event)\b|/dev/mouse\b')
errors = []
count = 0
for directory in ['src', 'include', 'libc/include', 'userland/base', 'userland/X11']:
    for path in (ROOT / directory).rglob('*'):
        if path.suffix not in ['.c', '.h'] or not path.is_file():
            continue
        if any(part in ['distfiles', '.git', 'build', '.internal'] for part in path.parts):
            continue
        count += 1
        for number, line in enumerate(path.read_text(errors='replace').splitlines(), 1):
            if retired.search(line):
                errors.append(f'{path.relative_to(ROOT)}:{number}: {line.strip()}')
if errors:
    raise SystemExit('\n'.join(errors))
header = (ROOT / 'include/uapi/zedbsd/console.h').read_text()
assert re.search(r'ZEDBSD_CONSOLE_ISATTY\s+_IO\(ZEDBSD_CONSOLE_IOC_GROUP, 13\)', header)
print(f'IN-T50 source PASS: {count} live C/header files; surviving ioctl 13 retained')
