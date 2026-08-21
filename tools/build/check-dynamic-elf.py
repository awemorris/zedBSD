#!/usr/bin/env python3
"""Reject dynamic ELF features that the first zedBSD rtld cannot consume."""

import argparse
import struct
from pathlib import Path

PT_LOAD, PT_INTERP, PT_DYNAMIC = 1, 3, 2
PT_GNU_STACK, PT_GNU_RELRO = 0x6474E551, 0x6474E552
PF_X, PF_W, PF_R = 1, 2, 4
ET_EXEC, ET_DYN = 2, 3
DT_NULL, DT_NEEDED, DT_HASH, DT_SONAME = 0, 1, 4, 14
DT_GNU_HASH = 0x6FFFFEF5
DT_RPATH = 15
DT_RUNPATH = 29
DT_VERSYM = 0x6FFFFFF0
DT_VERDEF, DT_VERDEFNUM = 0x6FFFFFFC, 0x6FFFFFFD
DT_VERNEED, DT_VERNEEDNUM = 0x6FFFFFFE, 0x6FFFFFFF
DT_TEXTREL, DT_FLAGS = 22, 30
DF_TEXTREL = 4

MACHINES = {
    "i386": (1, 3, "<", {0, 1, 2, 6, 7, 8, 35, 36}),
    "amd64": (2, 62, "<", {0, 1, 6, 7, 8, 16, 17, 36}),
    "aarch64": (2, 183, "<", {0, 257, 1025, 1026, 1027, 1028, 1029, 1031}),
    "sparcv9": (2, 43, ">", {0, 20, 21, 22, 32, 75, 77}),
}

PRIVATE_RTLD = {
	"__rtld_exports",
    "__rtld_dlclose", "__rtld_dlerror",
    "__rtld_dlopen", "__rtld_dlsym",
    "__rtld_dlvsym",
    "__rtld_fork_child", "__rtld_fork_parent",
    "__rtld_fork_prepare", "__rtld_process_fini",
    "__rtld_pthread_private", "__rtld_startup_init",
    "__rtld_thread_alloc", "__rtld_thread_attach",
    "__rtld_thread_free", "__tls_get_addr", "___tls_get_addr",
}


def fail(path, message):
    raise SystemExit(f"{path}: {message}")


def cstring(blob, offset, path, what):
    if offset < 0 or offset >= len(blob):
        fail(path, f"{what} string offset is outside its table")
    end = blob.find(b"\0", offset)
    if end < 0:
        fail(path, f"unterminated {what} string")
    try:
        return blob[offset:end].decode("ascii")
    except UnicodeDecodeError:
        fail(path, f"non-ASCII {what} string")


def unpack_table(data, offset, count, size, fmt, path, what):
    native = struct.calcsize(fmt)
    if size < native or offset > len(data) or count > (len(data) - offset) // size:
        fail(path, f"invalid {what} table")
    return [struct.unpack_from(fmt, data, offset + i * size) for i in range(count)]


def check(path, machine_name, role):
    data = path.read_bytes()
    elf_class, machine, endian, allowed_relocs = MACHINES[machine_name]
    if len(data) < 64 or data[:4] != b"\x7fELF" or data[4] != elf_class:
        fail(path, f"expected ELF{elf_class * 32}")
    if data[5] != (1 if endian == "<" else 2) or data[6] != 1:
        fail(path, "wrong byte order or ELF version")
    if elf_class == 1:
        ehfmt, phfmt, shfmt = endian+"16sHHIIIIIHHHHHH", endian+"IIIIIIII", endian+"IIIIIIIIII"
    else:
        ehfmt, phfmt, shfmt = endian+"16sHHIQQQIHHHHHH", endian+"IIQQQQQQ", endian+"IIQQQQIIQQ"
    eh = struct.unpack_from(ehfmt, data)
    e_type, e_machine = eh[1], eh[2]
    phoff, shoff = eh[5], eh[6]
    phentsize, phnum, shentsize, shnum, shstrndx = eh[9], eh[10], eh[11], eh[12], eh[13]
    expected_type = ET_DYN
    if e_machine != machine or e_type != expected_type:
        fail(path, f"wrong machine or ELF type for {role}")
    phdrs = unpack_table(data, phoff, phnum, phentsize, phfmt, path, "program-header")
    shdrs = unpack_table(data, shoff, shnum, shentsize, shfmt, path, "section-header")
    if shstrndx >= len(shdrs):
        fail(path, "invalid section-name table")

    def section_values(sh):
        return (sh[4], sh[5], sh[6], sh[9]) if elf_class == 2 else (sh[4], sh[5], sh[6], sh[9])

    shstr_off, shstr_size, _, _ = section_values(shdrs[shstrndx])
    if shstr_off > len(data) or shstr_size > len(data) - shstr_off:
        fail(path, "section-name table is outside file")
    shstr = data[shstr_off:shstr_off+shstr_size]
    sections = {}
    for sh in shdrs:
        name = cstring(shstr, sh[0], path, "section")
        off, size, link, entsize = section_values(sh)
        if off > len(data) or sh[1] != 8 and size > len(data) - off:
            fail(path, f"section {name} is outside file")
        sections[name] = (sh, data[off:off+size], link, entsize)
    for forbidden in (".relr.dyn",):
        if forbidden in sections:
            fail(path, f"unsupported section {forbidden}")
    if role != "version-definition" and ".gnu.version_d" in sections:
        fail(path, "unexpected symbol version definitions")
    if role != "version-consumer" and ".gnu.version_r" in sections:
        fail(path, "unexpected symbol version requirements")

    interps, stacks, relros, tls_segments = [], [], [], []
    temporary_plts = 0
    for ph in phdrs:
        if elf_class == 1:
            p_type, p_offset, p_vaddr, _, p_filesz, p_memsz, p_flags, _ = ph
        else:
            p_type, p_flags, p_offset, p_vaddr, _, p_filesz, p_memsz, _ = ph
        if p_type == PT_LOAD and p_flags & PF_W and p_flags & PF_X:
            if (machine_name != "sparcv9" or temporary_plts != 0 or
                    p_flags != PF_R | PF_W | PF_X or p_filesz == 0 or
                    p_filesz != p_memsz or p_memsz > 8192 or
                    p_offset % 8192 or p_vaddr % 8192):
                fail(path, "writable executable PT_LOAD")
            temporary_plts += 1
        if p_type == PT_INTERP:
            if p_offset > len(data) or p_filesz > len(data) - p_offset:
                fail(path, "PT_INTERP outside file")
            interps.append(data[p_offset:p_offset+p_filesz].rstrip(b"\0"))
        if p_type == PT_GNU_STACK:
            stacks.append(p_flags)
        if p_type == PT_GNU_RELRO:
            relros.append(ph)
        if p_type == 7:
            tls_segments.append(ph)
    if role == "program":
        if interps != [b"/lib/ld.so"]:
            fail(path, "dynamic program must use /lib/ld.so")
        if len(stacks) != 1 or stacks[0] != PF_R | PF_W:
            fail(path, "program requires one non-executable RW stack")
        if tls_segments:
            fail(path, "ET_EXEC TLS would relax to unsupported IE/LE access")
    elif interps:
        fail(path, "shared object must not contain PT_INTERP")
    if not relros:
        fail(path, "missing PT_GNU_RELRO")

    dynamic = sections.get(".dynamic")
    dynstr_entry = sections.get(".dynstr")
    if (dynamic is None or dynstr_entry is None or
            ".hash" not in sections and ".gnu.hash" not in sections):
        fail(path, "missing .dynamic, .dynstr, or symbol hash")
    _, dynblob, _, dynentsize = dynamic
    dynstr = dynstr_entry[1]
    dynfmt = endian + ("iI" if elf_class == 1 else "qQ")
    if dynentsize != struct.calcsize(dynfmt) or len(dynblob) % dynentsize:
        fail(path, "invalid dynamic table entry size")
    tags, needed, sonames, rpaths, runpaths = [], [], [], [], []
    for off in range(0, len(dynblob), dynentsize):
        tag, value = struct.unpack_from(dynfmt, dynblob, off)
        tags.append(tag)
        if tag == DT_NEEDED:
            needed.append(cstring(dynstr, value, path, "DT_NEEDED"))
        elif tag == DT_SONAME:
            sonames.append(cstring(dynstr, value, path, "DT_SONAME"))
        elif tag == DT_RPATH:
            rpaths.append(cstring(dynstr, value, path, "DT_RPATH"))
        elif tag == DT_RUNPATH:
            runpaths.append(cstring(dynstr, value, path, "DT_RUNPATH"))
        elif tag == DT_TEXTREL or tag == DT_FLAGS and value & DF_TEXTREL:
            fail(path, "text relocations are forbidden")
    if DT_NULL not in tags:
        fail(path, "unterminated dynamic table")

    gnu_hash_entry = sections.get(".gnu.hash")
    if role == "module" and (gnu_hash_entry is None or DT_GNU_HASH not in tags):
        fail(path, "test module must exercise GNU hash lookup")
    if role == "module" and ".hash" in sections:
        fail(path, "test module must remain GNU-hash-only")
    if role == "libc" and (gnu_hash_entry is None or ".hash" not in sections or
                           DT_GNU_HASH not in tags or DT_HASH not in tags):
        fail(path, "libc must exercise matching GNU and SysV hash tables")
    if gnu_hash_entry is not None:
        gnu_hash = gnu_hash_entry[1]
        if len(gnu_hash) < 16:
            fail(path, "truncated GNU hash header")
        buckets, symoffset, bloom_count, bloom_shift = struct.unpack_from(
            endian + "IIII", gnu_hash)
        word_size = 4 if elf_class == 1 else 8
        word_bits = word_size * 8
        if (buckets == 0 or bloom_count == 0 or
                bloom_count & (bloom_count - 1) or bloom_shift >= word_bits):
            fail(path, "invalid GNU hash header")
        prefix = 16 + bloom_count * word_size + buckets * 4
        if prefix > len(gnu_hash) or (len(gnu_hash) - prefix) % 4:
            fail(path, "invalid GNU hash table size")
        dynsym = sections.get(".dynsym")
        if dynsym is None or dynsym[3] == 0 or len(dynsym[1]) % dynsym[3]:
            fail(path, "GNU hash requires a valid dynsym")
        symbol_count = len(dynsym[1]) // dynsym[3]
        chain_count = (len(gnu_hash) - prefix) // 4
        if symoffset > symbol_count or chain_count < symbol_count - symoffset:
            fail(path, "GNU hash chain does not cover dynsym")
        bucket_fmt = endian + "I"
        bucket_base = 16 + bloom_count * word_size
        for index in range(buckets):
            symbol = struct.unpack_from(bucket_fmt, gnu_hash,
                                        bucket_base + index * 4)[0]
            if symbol and not symoffset <= symbol < symbol_count:
                fail(path, "GNU hash bucket is outside dynsym")
    if role == "interpreter" and needed:
        fail(path, "ld.so must not have dependencies")
    if role == "libc" and (sonames != ["libc.so"] or needed):
        fail(path, "libc.so has wrong SONAME or dependency")
    if role == "module" and (len(sonames) != 1 or
                              needed != ["rpathdep.so"] or
                              rpaths or
                              runpaths != ["$ORIGIN/alt"]):
        fail(path, "module needs its tested dependency and RUNPATH")
    if role == "rpath-module" and (len(sonames) != 1 or
                                    needed != ["rpathdep.so"] or
                                    rpaths != ["$ORIGIN/alt"] or runpaths):
        fail(path, "legacy module needs its tested dependency and RPATH")
    if role == "version-definition" and (
            sonames != ["verstest.so"] or needed or
            ".gnu.version" not in sections or ".gnu.version_d" not in sections or
            DT_VERSYM not in tags or DT_VERDEF not in tags or
            DT_VERDEFNUM not in tags):
        fail(path, "version definition fixture has the wrong contract")
    if role == "version-consumer" and (
            sonames != ["versuse.so"] or needed != ["verstest.so"] or
            ".gnu.version" not in sections or ".gnu.version_r" not in sections or
            DT_VERSYM not in tags or DT_VERNEED not in tags or
            DT_VERNEEDNUM not in tags):
        fail(path, "version consumer fixture has the wrong contract")
    if role == "program" and needed != ["libc.so"]:
        fail(path, "test program must depend only on libc.so")

    seen_tlsdesc = False
    for name, (sh, blob, link, entsize) in sections.items():
        if sh[1] not in (9, 4):
            continue
        expected = ((12 if elf_class == 1 else 24) if sh[1] == 4 else
                    (8 if elf_class == 1 else 16))
        if entsize != expected or len(blob) % entsize:
            fail(path, f"invalid relocation section {name}")
        for off in range(0, len(blob), entsize):
            if elf_class == 1:
                _, info = struct.unpack_from(endian+"II", blob, off)
                kind = info & 0xff
            else:
                _, info = struct.unpack_from(endian+"QQ", blob, off)
                kind = info & 0xffffffff
            if kind not in allowed_relocs:
                fail(path, f"unsupported relocation {kind} in {name}")
            if ((machine_name == "amd64" and kind == 36) or
                    (machine_name == "aarch64" and kind == 1031)):
                seen_tlsdesc = True
            if role == "interpreter" and kind not in ({8} if machine_name in ("i386", "amd64") else {22} if machine_name == "sparcv9" else {1027}):
                fail(path, f"ld.so bootstrap relocation {kind} is not relative")

    if role == "module" and machine_name in ("amd64", "aarch64") and not seen_tlsdesc:
        fail(path, "test module does not exercise TLSDESC")

    if role in ("libc", "module", "rpath-module", "version-definition",
                "version-consumer") and ".dynsym" in sections:
        sh, syms, link, entsize = sections[".dynsym"]
        if link >= len(shdrs):
            fail(path, "invalid dynsym string-table link")
        strings = dynstr
        symfmt = endian + ("IIIBBH" if elf_class == 1 else "IBBHQQ")
        if entsize != struct.calcsize(symfmt) or len(syms) % entsize:
            fail(path, "invalid dynsym")
        for off in range(0, len(syms), entsize):
            sym = struct.unpack_from(symfmt, syms, off)
            name_offset = sym[0]
            shndx = sym[5] if elf_class == 1 else sym[3]
            bind = (sym[3] if elf_class == 1 else sym[1]) >> 4
            if shndx == 0 and bind == 1:
                name = cstring(strings, name_offset, path, "symbol")
                if role in ("module", "rpath-module") and name not in {
                    "__tls_get_addr", "___tls_get_addr",
                    "__rtld_exports", "dlopen", "dlclose",
                    "rpath_dependency_value",
                }:
                    fail(path, f"unexpected module undefined symbol {name}")
                if role == "version-consumer" and name != "versioned_value":
                    fail(path, f"unexpected version consumer symbol {name}")
                if role == "version-definition":
                    fail(path, f"unexpected version definition symbol {name}")
                if role == "libc" and name not in PRIVATE_RTLD:
                    fail(path, f"unexpected libc undefined symbol {name}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--machine", choices=MACHINES, required=True)
    parser.add_argument("--role", choices=("interpreter", "libc", "module",
                                            "rpath-module", "version-definition",
                                            "version-consumer", "program"),
                        required=True)
    parser.add_argument("elf", type=Path)
    args = parser.parse_args()
    check(args.elf, args.machine, args.role)


if __name__ == "__main__":
    main()
