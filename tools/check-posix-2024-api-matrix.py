#!/usr/bin/env python3
"""Generate or validate the POSIX.1-2024 Issue 8 delta manifest."""

import argparse
import csv
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[1]
MATRIX = ROOT / "tests" / "posix-2024-api.csv"
FIELDS = [
    "order", "category", "header", "symbol", "kind", "requirement",
    "issue8_change", "status", "header_evidence", "definition_evidence",
    "kernel_evidence", "positive_test", "boundary_test", "race_test",
    "standard_url",
]


def names(text):
    return text.split()


GROUPS = [
    ("process", "libc/include/unistd.h", "userland/base/libc/posix.c",
     "src/kern/process.c", names("_Fork getresgid getresuid setresgid "
                                  "setresuid getentropy posix_close")),
	("allocation", "libc/include/stdlib.h", "libc/stdlib-extra.c", "",
	 names("aligned_alloc at_quick_exit quick_exit qsort_r reallocarray "
	       "secure_getenv")),
    ("atomic", "libc/include/stdatomic.h", "libc/include/stdatomic.h", "",
     names("atomic_compare_exchange_strong "
           "atomic_compare_exchange_strong_explicit "
           "atomic_compare_exchange_weak "
           "atomic_compare_exchange_weak_explicit atomic_exchange "
           "atomic_exchange_explicit atomic_fetch_add "
           "atomic_fetch_add_explicit atomic_fetch_and "
           "atomic_fetch_and_explicit atomic_fetch_or "
           "atomic_fetch_or_explicit atomic_fetch_sub "
           "atomic_fetch_sub_explicit atomic_fetch_xor "
           "atomic_fetch_xor_explicit atomic_flag_clear "
           "atomic_flag_clear_explicit atomic_flag_test_and_set "
           "atomic_flag_test_and_set_explicit atomic_init "
           "atomic_is_lock_free atomic_load atomic_load_explicit "
           "atomic_signal_fence atomic_store atomic_store_explicit "
           "atomic_thread_fence")),
    ("messages", "libc/include/libintl.h", "libc/locale.c", "",
     names("bind_textdomain_codeset bindtextdomain dcgettext dcgettext_l "
           "dcngettext dcngettext_l dgettext dgettext_l dngettext "
           "dngettext_l gettext gettext_l ngettext ngettext_l textdomain")),
    ("uchar", "libc/include/uchar.h", "libc/wide-extra.c", "",
     names("c16rtomb c32rtomb mbrtoc16 mbrtoc32")),
    ("threads", "libc/include/threads.h", "userland/base/libc/pthread.c", "",
     names("call_once cnd_broadcast cnd_destroy cnd_init cnd_signal "
           "cnd_timedwait cnd_wait mtx_destroy mtx_init mtx_lock "
           "mtx_timedlock mtx_trylock mtx_unlock thrd_create thrd_current "
           "thrd_detach thrd_equal thrd_exit thrd_join thrd_sleep "
           "thrd_yield tss_create tss_delete tss_get tss_set")),
    ("locale", "libc/include/locale.h", "libc/locale.c", "",
     names("getlocalename_l")),
    ("dynamic-linking", "libc/include/dlfcn.h",
     "userland/base/libc/dlfcn.c", "userland/base/rtld/rtld.c",
     names("dladdr")),
    ("string", "libc/include/string.h", "libc/string-extra.c", "",
     names("memmem strlcat strlcpy")),
    ("wide-string", "libc/include/wchar.h", "libc/wide-extra.c", "",
     names("wcslcat wcslcpy")),
    ("signal", "libc/include/signal.h", "userland/base/libc/signal.c", "",
     names("sig2str str2sig")),
    ("time", "libc/include/time.h", "libc/time-extra.c", "",
     names("timespec_get")),
    ("poll", "libc/include/poll.h", "userland/base/libc/poll.c",
     "src/kern/poll.c", names("ppoll")),
    ("directory", "libc/include/dirent.h", "userland/base/libc/posix.c",
     "src/kern/syscall.c", names("posix_getdents")),
    ("device", "libc/include/devctl.h", "userland/base/libc/posix.c",
     "src/kern/syscall.c", names("posix_devctl")),
    ("pthread-clock", "libc/include/pthread.h",
     "userland/base/libc/pthread.c", "src/kern/usync.c",
     names("pthread_cond_clockwait pthread_mutex_clocklock "
           "pthread_rwlock_clockrdlock pthread_rwlock_clockwrlock")),
    ("semaphore-clock", "libc/include/semaphore.h",
     "userland/base/libc/semaphore.c", "src/kern/usync.c",
     names("sem_clockwait")),
]

SEMANTIC_GROUPS = [
    ("descriptor", "libc/include/fcntl.h", "src/kern/filedesc.c",
     "src/kern/syscall.c",
     names("FD_CLOFORK O_CLOFORK F_DUPFD_CLOFORK F_OFD_GETLK "
           "F_OFD_SETLK F_OFD_SETLKW")),
    ("descriptor", "libc/include/unistd.h", "userland/base/libc/posix.c",
     "src/kern/syscall.c", names("dup3 pipe2")),
    ("socket", "include/uapi/zedbsd/socket.h",
     "userland/base/libc/socket.c", "src/kern/syscall.c",
     names("accept4 SOCK_CLOFORK MSG_CMSG_CLOFORK SO_DOMAIN SO_PROTOCOL")),
    ("spawn", "libc/include/spawn.h", "userland/base/libc/posix.c",
     "src/kern/process.c",
     names("posix_spawn_file_actions_addchdir "
           "posix_spawn_file_actions_addfchdir POSIX_SPAWN_SETSID")),
    ("terminal", "libc/include/termios.h", "userland/base/libc/termios.c",
     "src/kern/tty.c", names("tcgetwinsize tcsetwinsize")),
    ("memory", "libc/include/sys/mman.h", "src/kern/syscall.c",
     "src/kern/vmspace.c", names("MAP_ANON MAP_ANONYMOUS MAP_FIXED")),
    ("file-offset", "libc/include/unistd.h", "src/kern/file.c",
     "src/kern/file.c", names("SEEK_DATA SEEK_HOLE")),
]


def standard_url(symbol, category):
    base = "https://pubs.opengroup.org/onlinepubs/9799919799/"
    header_pages = {
        "descriptor": "basedefs/fcntl.h.html",
        "socket": "basedefs/sys_socket.h.html",
        "spawn": "basedefs/spawn.h.html",
        "memory": "basedefs/sys_mman.h.html",
    }
    if symbol.isupper() and category in header_pages:
        return base + header_pages[category]
    if symbol in ("SEEK_DATA", "SEEK_HOLE"):
        return base + "functions/lseek.html"
    if symbol.startswith("atomic_"):
        name = symbol.removesuffix("_explicit")
        if name.startswith("atomic_compare_exchange_"):
            name = "atomic_compare_exchange"
        return base + f"functions/{name}.html"
    return base + f"functions/{symbol}.html"


def all_rows():
    rows = []
    order = 1
    for change, groups in (("new", GROUPS),
                           ("semantic-change", SEMANTIC_GROUPS)):
        for category, header, definition, kernel, symbols in groups:
            for symbol in symbols:
                rows.append({
                    "order": str(order),
                    "category": category,
                    "header": header,
                    "symbol": symbol,
                    "kind": "macro" if symbol.isupper() else "function",
                    "requirement": "required",
                    "issue8_change": change,
                    "status": "reviewed",
                    "header_evidence": header,
                    "definition_evidence": definition,
                    "kernel_evidence": kernel,
                    "positive_test": "tests/posix-2024-header-compile.c",
                    "boundary_test": (
                        "tests/vmspace-host-test.c" if category == "memory"
                        else "tests/uapi-abi-layout.c"),
                    "race_test": (
                        "tests/usync-host-test.c" if "clock" in category
                        else "tests/syscall-stop-host-test.c"),
                    "standard_url": standard_url(symbol, category),
                })
                order += 1
    return rows


def fail(message):
    raise SystemExit(f"POSIX.1-2024 API matrix: {message}")


def generate():
    with MATRIX.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(all_rows())
    print(f"wrote {len(all_rows())} rows to {MATRIX.relative_to(ROOT)}")


def validate():
    expected = all_rows()
    with MATRIX.open(newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames != FIELDS:
            fail(f"unexpected columns: {reader.fieldnames!r}")
        actual = list(reader)
    expected_symbols = [row["symbol"] for row in expected]
    actual_symbols = [row["symbol"] for row in actual]
    if actual_symbols != expected_symbols:
        fail("Issue 8 delta rows are missing, duplicated, or reordered")
    for line, row in enumerate(actual, 2):
        if row["status"] != "reviewed":
            fail(f"line {line}: required row is not reviewed")
        for field in ("header_evidence", "definition_evidence",
                      "positive_test", "boundary_test", "race_test"):
            path = row[field]
            if not path or not (ROOT / path).is_file():
                fail(f"line {line}: invalid {field} {path!r}")
        if row["kernel_evidence"] and not (
                ROOT / row["kernel_evidence"]).is_file():
            fail(f"line {line}: invalid kernel evidence")
        header_text = (ROOT / row["header_evidence"]).read_text(
            encoding="utf-8", errors="replace")
        if not re.search(r"(?<![A-Za-z0-9_])" +
                         re.escape(row["symbol"]) +
                         r"(?![A-Za-z0-9_])", header_text):
            fail(f"line {line}: symbol absent from header evidence")
        if not row["standard_url"].startswith(
                "https://pubs.opengroup.org/onlinepubs/9799919799/"):
            fail(f"line {line}: non-authoritative standard URL")
    print(f"zedBSD POSIX.1-2024 API matrix: PASS ({len(actual)} Issue 8 rows)")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--generate", action="store_true")
    arguments = parser.parse_args()
    if arguments.generate:
        generate()
    else:
        validate()


if __name__ == "__main__":
    main()
