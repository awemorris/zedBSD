#!/usr/bin/env python3
"""Compares the IN6_IS_ADDR_* macros of include/libc/netinet/in.h with the
host C library's, over a set of addresses (ws034-p017).

Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
"""
from pathlib import Path
import re
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[3]
ADDRESSES = ["::", "::1", "::2", "fe80::1", "febf::1", "fec0::1", "feff::1",
             "ff02::1", "ff01::1", "ff05::1", "ff08::1", "ff0e::1", "ff12::1",
             "::ffff:1.2.3.4", "::1.2.3.4", "::0.0.0.1", "2001:db8::1", "1::",
             "::ffff:0:0"]


def main():
    ours = (REPO / "include/libc/netinet/in.h").read_text()
    body = ours[ours.index("#define __IN6_BYTE"):ours.rindex("#endif")]
    names = re.findall(r"#define (IN6_IS_ADDR_\w+)", body)
    for name in names:
        body = re.sub(r"\b" + name + r"\b", "z_" + name, body)
    source = ("#include <stdio.h>\n#include <arpa/inet.h>\n#include <netinet/in.h>\n"
              "#include <assert.h>\n" + body + "\nint main(void) { int bad = 0;\n")
    for address in ADDRESSES:
        for name in names:
            source += ('{ struct in6_addr x; assert(inet_pton(AF_INET6, "%s", &x) == 1);'
                       ' if (!!z_%s(&x) != !!%s(&x)) { printf("DIFF %s %s\\n"); bad = 1; } }\n'
                       % (address, name, name, name, address))
    source += ('printf(bad ? "in6-macros: FAIL\\n" : "in6-macros: PASS (%d addresses, %d macros)\\n");'
               ' return bad; }\n' % (len(ADDRESSES), len(names)))
    with tempfile.TemporaryDirectory() as directory:
        path = Path(directory) / "test.c"
        path.write_text(source)
        subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", str(path), "-o",
                        str(Path(directory) / "test")], check=True)
        subprocess.run([str(Path(directory) / "test")], check=True)


if __name__ == "__main__":
    main()
