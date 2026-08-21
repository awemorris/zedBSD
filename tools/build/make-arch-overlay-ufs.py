#!/usr/bin/env python3
"""Create an architecture-specific UFS1 root filesystem image."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import os
import re
import shutil
import tempfile
from pathlib import Path

from ufs1_format import create

DESTINATION=re.compile(
    r"/(bin|lib|etc|var|home|usr)(?:/[A-Za-z0-9_][A-Za-z0-9_.-]{0,254}){1,4}")


def parse_files(specifications: list[str]) -> dict[str,Path]:
    result={}
    for specification in specifications:
        if "=" not in specification: raise SystemExit("--file requires DESTINATION=SOURCE")
        destination,source_text=specification.split("=",1); source=Path(source_text)
        if not DESTINATION.fullmatch(destination): raise SystemExit(f"invalid destination: {destination}")
        if destination in result: raise SystemExit(f"duplicate destination: {destination}")
        if not source.is_file(): raise SystemExit(f"missing input: {source}")
        result[destination]=source
    if "/bin/sh" not in result: raise SystemExit("profile manifest must contain /bin/sh")
    return result


def build(args: argparse.Namespace) -> None:
    files=parse_files(args.file)
    if args.output.exists() and not args.force: raise SystemExit(f"output exists: {args.output}")
    if args.size_mib<16: raise SystemExit("architecture image must be at least 16 MiB")
    args.output.parent.mkdir(parents=True,exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="zedbsd-arch-ufs-") as work_text:
        root=Path(work_text)
        for directory in ("bin", "lib", "etc", "var/run", "root", "home",
                          "usr/bin", "dev", "boot", "tmp", "run", "shm"):
            (root/directory).mkdir(parents=True, exist_ok=True)
        (root/"tmp").chmod(0o1777)
        (root/"shm").chmod(0o1777)
        (root/"lib"/"arch.id").write_text(args.profile+"\n",encoding="ascii")
        (root/"etc"/"zedbsd-root").write_text(
            "zedBSD ufs1 root v1\n", encoding="ascii")
        for destination,source in files.items():
            target=root/destination.lstrip("/"); target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(source,target)
        for item in args.mode:
            destination, mode_text = item.split("=", 1)
            target = root / destination.lstrip("/")
            if destination not in files: raise SystemExit(f"mode destination is not a manifest file: {destination}")
            target.chmod(int(mode_text, 8))
        temporary=args.output.with_name(args.output.name+".tmp")
        try:
            temporary.write_bytes(create(args.size_mib*1024*1024,root))
            os.replace(temporary,args.output)
        finally:
            if temporary.exists(): temporary.unlink()


def main() -> None:
    parser=argparse.ArgumentParser()
    parser.add_argument("--profile",choices=("i386","amd64","aarch64"),required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--size-mib",type=int,default=16)
    parser.add_argument("--file",action="append",default=[])
    parser.add_argument("--mode",action="append",default=[])
    parser.add_argument("--force",action="store_true")
    build(parser.parse_args())


if __name__=="__main__": main()
