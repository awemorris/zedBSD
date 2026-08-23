#!/usr/bin/env python3
"""Generate the ISO C17 and common BSD libc function audit table."""

import argparse
import csv
from pathlib import Path
import re
import subprocess


ISO_FUNCTIONS = {
    "ctype.h": """
        isalnum isalpha isblank iscntrl isdigit isgraph islower isprint
        ispunct isspace isupper isxdigit tolower toupper
    """.split(),
    "fenv.h": """
        feclearexcept fegetenv fegetexceptflag fegetround feholdexcept
        feraiseexcept fesetenv fesetexceptflag fesetround fetestexcept
        feupdateenv
    """.split(),
    "inttypes.h": """
        imaxabs imaxdiv strtoimax strtoumax wcstoimax wcstoumax
    """.split(),
    "locale.h": "localeconv setlocale".split(),
    "setjmp.h": "longjmp setjmp".split(),
    "signal.h": "raise signal".split(),
    "stdio.h": """
        clearerr fclose feof ferror fflush fgetc fgetpos fgets fopen fprintf
        fputc fputs fread freopen fscanf fseek fsetpos ftell fwrite getc
        getchar perror printf putc putchar puts remove rename rewind scanf
        setbuf setvbuf snprintf sprintf sscanf tmpfile tmpnam ungetc vfprintf
        vfscanf vprintf vscanf vsnprintf vsprintf vsscanf
    """.split(),
    "stdlib.h": """
        _Exit abort abs aligned_alloc atexit at_quick_exit atof atoi atol atoll
        bsearch calloc div exit free getenv labs ldiv llabs lldiv malloc mblen
        mbstowcs mbtowc qsort quick_exit rand realloc srand strtod strtof
        strtol strtold strtoll strtoul strtoull system wcstombs wctomb
    """.split(),
    "string.h": """
        memchr memcmp memcpy memmove memset strcat strchr strcmp strcoll strcpy
        strcspn strerror strlen strncat strncmp strncpy strpbrk strrchr strspn
        strstr strtok strxfrm
    """.split(),
    "time.h": """
        asctime clock ctime difftime gmtime localtime mktime strftime time
        timespec_get
    """.split(),
    "uchar.h": "c16rtomb c32rtomb mbrtoc16 mbrtoc32".split(),
    "wchar.h": """
        btowc fgetwc fgetws fputwc fputws fwide fwprintf fwscanf getwc
        getwchar mbrlen mbrtowc mbsinit mbsrtowcs putwc putwchar swprintf
        swscanf ungetwc vfwprintf vfwscanf vswprintf vswscanf vwprintf vwscanf
        wcrtomb wcscat wcschr wcscmp wcscoll wcscpy wcscspn wcsftime wcslen
        wcsncat wcsncmp wcsncpy wcspbrk wcsrchr wcsrtombs wcsspn wcsstr
        wcstod wcstof wcstok wcstol wcstold wcstoll wcstoul wcstoull wcsxfrm
        wctob wmemcmp wmemchr wmemcpy wmemmove wmemset wprintf wscanf
    """.split(),
    "wctype.h": """
        iswalnum iswalpha iswblank iswcntrl iswdigit iswgraph iswlower
        iswprint iswpunct iswspace iswupper iswctype iswxdigit towctrans
        towlower towupper wctrans wctype
    """.split(),
}


def math_functions():
    unary = """
        acos acosh asin asinh atan atanh cbrt ceil cos cosh erf erfc exp exp2
        expm1 fabs floor ilogb lgamma log log10 log1p log2 logb nearbyint rint
        round sin sinh sqrt tan tanh tgamma trunc
    """.split()
    binary = "atan2 copysign fdim fmax fmin fmod hypot nextafter pow remainder".split()
    ternary = "fma".split()
    integer_exponent = "ldexp scalbln scalbn".split()
    remainder_quotient = "remquo".split()
    decompose = "frexp modf".split()
    integer_result = "llrint llround lrint lround".split()
    nan_functions = "nan".split()
    next_toward = "nexttoward".split()
    result = []
    for name in (unary + binary + ternary + integer_exponent +
                 remainder_quotient + decompose + integer_result +
                 nan_functions + next_toward):
        result.extend((name, name + "f", name + "l"))
    return result


ISO_FUNCTIONS["math.h"] = math_functions()


BSD_FUNCTIONS = {
    "err.h": """
        err err_set_exit err_set_file errc errx verr verrc verrx warn warnc
        warnx vwarn vwarnc vwarnx
    """.split(),
    "stdio.h": """
        asprintf fgetln fmtcheck fpurge funopen setbuffer setlinebuf vasprintf
    """.split(),
    "stdlib.h": """
        arc4random arc4random_buf arc4random_uniform getprogname heapsort
        mergesort qsort_r reallocarray recallocarray reallocf setprogname
        srandomdev strtonum
    """.split(),
    "string.h": """
        memmem mempcpy memrchr memset_explicit strcasestr strchrnul strlcat
        strlcpy strmode strnstr strsep strverscmp timingsafe_bcmp
        timingsafe_memcmp
    """.split(),
    "strings.h": """
        bcmp bcopy bzero explicit_bzero ffs ffsl ffsll fls flsl flsll index
        rindex
    """.split(),
}


FIELDS = [
    "order", "family", "header", "symbol", "kind", "requirement",
    "implementation_status", "header_evidence", "implementation_source",
    "test_evidence", "implementation_notes", "review_status",
    "review_findings", "fix_status", "standard_url",
]


def defined_symbols(repo):
    objects = set()
    build = repo / "build"
    if build.exists():
        for candidate in build.rglob("*.o"):
            text = candidate.relative_to(repo).as_posix()
            if "/libc/" in text or "/softfloat/" in text:
                objects.add(candidate)
    if not objects:
        return set()
    result = subprocess.run(
        ["nm", "-g", "--defined-only", *map(str, sorted(objects))],
        check=False, text=True, stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL)
    symbols = set()
    for line in result.stdout.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[-2] in {
                "A", "B", "C", "D", "G", "R", "S", "T", "V", "W"}:
            symbols.add(fields[-1])
    return symbols


def header_evidence(repo, header, symbol):
    path = repo / "libc/include" / header
    if not path.exists():
        return "", False, False
    text = path.read_text(encoding="utf-8", errors="replace")
    present = re.search(
        r"(?<![A-Za-z0-9_])" + re.escape(symbol) + r"(?![A-Za-z0-9_])",
        text) is not None
    if not present and header == "math.h":
        base = symbol[:-1] if symbol.endswith(("f", "l")) else symbol
        present = re.search(
            r"ZEDBSD_MATH_(?:UNARY|BINARY)\(" + re.escape(base) + r"\)",
            text) is not None
    macro = re.search(
        r"^\s*#\s*define\s+" + re.escape(symbol) + r"\b", text,
        re.MULTILINE) is not None
    return str(path.relative_to(repo)) if present else "", present, macro


def source_evidence(repo, symbol):
    pattern = re.compile(
        r"(?m)^[A-Za-z_][^;{}]*\b" + re.escape(symbol) + r"\s*\([^;]*\)\s*\{")
    exact = []
    matches = []
    roots = [
        repo / "libc", repo / "userland/base/libc",
        repo / "src/softfloat",
    ]
    for root in roots:
        if not root.exists():
            continue
        for path in root.rglob("*.c"):
            if path.stem == symbol:
                exact.append(str(path.relative_to(repo)))
                continue
            try:
                if pattern.search(path.read_text(
                        encoding="utf-8", errors="replace")):
                    matches.append(str(path.relative_to(repo)))
            except OSError:
                pass
    return "|".join((exact + matches)[:4])


def make_rows(repo):
    defined = defined_symbols(repo)
    rows = []
    catalogs = (("ISO-C17", ISO_FUNCTIONS), ("BSD", BSD_FUNCTIONS))
    for family, catalog in catalogs:
        for header in sorted(catalog):
            for symbol in sorted(set(catalog[header]), key=str.lower):
                evidence, declared, macro = header_evidence(repo, header, symbol)
                source = source_evidence(repo, symbol)
                kind = "macro" if symbol == "setjmp" or macro else "function"
                implemented = declared and (macro or symbol in defined)
                status = "implemented" if implemented else (
                    "declared-only" if declared else "missing")
                if family == "ISO-C17":
                    url = "https://www.open-std.org/jtc1/sc22/wg14/www/docs/n2176.pdf"
                    requirement = "required"
                else:
                    url = ("https://man.freebsd.org/cgi/man.cgi?query=" + symbol +
                           "&sektion=3&manpath=FreeBSD+15.1-RELEASE")
                    requirement = "common-extension"
                rows.append({
                    "family": family,
                    "header": f"<{header}>",
                    "symbol": symbol,
                    "kind": kind,
                    "requirement": requirement,
                    "implementation_status": status,
                    "header_evidence": evidence,
                    "implementation_source": source,
                    "test_evidence": "",
                    "implementation_notes": "",
                    "review_status": "pending",
                    "review_findings": "",
                    "fix_status": "pending",
                    "standard_url": url,
                })
    for index, row in enumerate(rows, 1):
        row["order"] = index
    return rows


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--repo", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    rows = make_rows(args.repo.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    counts = {}
    for row in rows:
        key = row["implementation_status"]
        counts[key] = counts.get(key, 0) + 1
    print(f"wrote {len(rows)} libc interfaces to {args.output}")
    for key in sorted(counts):
        print(f"{key}: {counts[key]}")


if __name__ == "__main__":
    main()
