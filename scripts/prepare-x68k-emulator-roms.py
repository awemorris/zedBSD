#!/usr/bin/env python3
"""Prepare external, redistributable-compatible X68030 emulator ROM inputs.

This tool never downloads inputs and must not be pointed at the repository as
its output directory.  It combines Sharp's separately supplied IPLROM30 with
XEiJ's replacement CGROM and open SCSIINROM implementation for local emulator
testing.  The generated files do not have the checksums of dumped hardware
CGROM/SCSI ROMs, so MAME is expected to print checksum warnings.
"""

import argparse
import ast
import hashlib
import json
import shutil
import sys
import zipfile
from pathlib import Path


IPLROM30_SIZE = 128 * 1024
IPLROM30_SHA1 = "239e9124568c862c31d9ec0605e32373ea74b86a"
CGROM_SIZE = 768 * 1024
SCSIINROM_SIZE = 8 * 1024
XEIJSCSI_MARKER = 'public static final byte[] SCSI16IN = "'


def digest(data: bytes, algorithm: str = "sha256") -> str:
    return hashlib.new(algorithm, data).hexdigest()


def fail(message: str) -> None:
    raise SystemExit("X68k emulator ROM preparation: " + message)


def decode_xeij_scsiinrom(java_source: str) -> bytes:
    """Decode XEiJ's ISO-8859-1 Java string and pad the 8 KiB ROM."""
    try:
        escaped = java_source.split(XEIJSCSI_MARKER, 1)[1].split(
            '".getBytes', 1)[0]
        text = ast.literal_eval('"' + escaped + '"')
        payload = text.encode("latin-1")
    except (IndexError, SyntaxError, UnicodeEncodeError, ValueError) as exc:
        raise ValueError("cannot decode XEiJ SCSI16IN source") from exc
    if not payload or len(payload) > SCSIINROM_SIZE:
        raise ValueError(f"invalid XEiJ SCSI16IN length {len(payload)}")
    if b"JSCSIIN" not in payload[:64]:
        raise ValueError("XEiJ SCSI16IN signature is absent")
    return payload + b"\xff" * (SCSIINROM_SIZE - len(payload))


def read_xeij(zip_path: Path) -> tuple[bytes, bytes, bytes]:
    try:
        with zipfile.ZipFile(zip_path) as archive:
            cgrom = archive.read("data/CGROM_XEiJ.DAT")
            source = archive.read("xeij/ROM.java").decode("utf-8")
            license_text = archive.read("data/license_XEiJ.txt")
    except (KeyError, OSError, UnicodeDecodeError, zipfile.BadZipFile) as exc:
        fail(f"invalid XEiJ archive {zip_path}: {exc}")
    if len(cgrom) != CGROM_SIZE:
        fail(f"XEiJ CGROM has unexpected size {len(cgrom)}")
    try:
        scsiinrom = decode_xeij_scsiinrom(source)
    except ValueError as exc:
        fail(str(exc))
    return cgrom, scsiinrom, license_text


def checked_output(path: Path, repository: Path | None) -> Path:
    output = path.resolve()
    if output == Path("/") or output == Path.home().resolve():
        fail(f"unsafe output directory {output}")
    if repository is not None:
        repo = repository.resolve()
        if output == repo or repo in output.parents:
            fail("output directory must be outside the source repository")
    return output


def prepare(args: argparse.Namespace) -> None:
    iplrom = args.iplrom30.read_bytes()
    if len(iplrom) != IPLROM30_SIZE:
        fail(f"IPLROM30 has unexpected size {len(iplrom)}")
    if digest(iplrom, "sha1") != IPLROM30_SHA1:
        fail("IPLROM30 does not match Sharp's published X68030 image")
    cgrom, scsiinrom, xeij_license = read_xeij(args.xeij_zip)
    sharp_license = args.sharp_license.read_bytes()
    if not sharp_license:
        fail("Sharp license file is empty")

    output = checked_output(args.output_dir, args.repository)
    output.mkdir(parents=True, exist_ok=True)
    license_dir = output / "licenses"
    license_dir.mkdir(exist_ok=True)
    (license_dir / "XEiJ-license.txt").write_bytes(xeij_license)
    shutil.copyfile(args.sharp_license, license_dir / args.sharp_license.name)

    # x68030 is a clone of MAME's x68000 ROM set.  Populate both directories
    # so this also works with MAME releases that resolve merged ROMs strictly.
    for set_name in ("x68000", "x68030"):
        set_dir = output / set_name
        set_dir.mkdir(exist_ok=True)
        (set_dir / "iplrom30.dat").write_bytes(iplrom)
        (set_dir / "cgrom.dat").write_bytes(cgrom)
        (set_dir / "scsiinrom.dat").write_bytes(scsiinrom)

    manifest = {
        "format": "zedbsd-x68k-emulator-rom-inputs-v1",
        "warning": "CGROM/SCSIINROM are XEiJ replacements, not hardware dumps",
        "inputs": {
            "iplrom30_sha256": digest(iplrom),
            "xeij_zip_sha256": digest(args.xeij_zip.read_bytes()),
            "sharp_license_sha256": digest(sharp_license),
        },
        "outputs": {
            "iplrom30.dat": {"bytes": len(iplrom), "sha256": digest(iplrom)},
            "cgrom.dat": {"bytes": len(cgrom), "sha256": digest(cgrom)},
            "scsiinrom.dat": {
                "bytes": len(scsiinrom),
                "sha256": digest(scsiinrom),
            },
        },
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(f"prepared external X68030 emulator ROM inputs in {output}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--iplrom30", type=Path, required=True)
    parser.add_argument("--sharp-license", type=Path, required=True)
    parser.add_argument("--xeij-zip", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument(
        "--repository",
        type=Path,
        help="reject output beneath this repository (recommended)",
    )
    return parser.parse_args()


if __name__ == "__main__":
    try:
        prepare(parse_args())
    except OSError as exc:
        fail(str(exc))
