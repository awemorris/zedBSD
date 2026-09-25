#!/usr/bin/env python3
"""MAC-T001 target Variant configuration fixture."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import importlib.util
import subprocess
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
MENU_PATH = REPO / "tools" / "menuconfig.py"
SPEC = importlib.util.spec_from_file_location("zedbsd_menuconfig", MENU_PATH)
if SPEC is None or SPEC.loader is None:
    raise SystemExit("MAC-T001: cannot load tools/menuconfig.py")
menu = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(menu)
USER_PROGRAM_ROWS = menu.user_program_rows()
menu.user_program_rows = lambda: USER_PROGRAM_ROWS


def fail(message: str) -> None:
    raise SystemExit(f"MAC-T001: {message}")


def make_result(config: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        ["make", "--no-print-directory", "-s",
         f"ZEDBSD_CONFIG={config}", "validate-image-config"],
        cwd=REPO, check=False, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def check_packages() -> None:
    """Checks what the package menu offers and what choosing from it does.

    A package that names a requirement no other package provides cannot be
    chosen at all, and a package filed under a category the menu does not
    list cannot be reached, so both are faults in the metadata rather than
    in the menu.
    """
    rows = USER_PROGRAM_ROWS
    provided = {row[5] for row in rows if row[5]}
    for row in rows:
        for requirement in row[6].split():
            if requirement not in provided:
                fail(f"{row[0]} requires {requirement}, which nothing provides")

    offered = set()
    for _label, group in menu.PACKAGE_CATEGORIES:
        offered.update(row[0] for row in rows if row[4] == group)
    for row in rows:
        if row[4].startswith("packages/") and row[0] not in offered:
            fail(f"{row[0]} is filed under {row[4]}, which the menu omits")

    # Where each package is filed, and that the menu reaches it there.
    expected_group = {
        "noct": "packages/lang",
        "remacs": "packages/editors",
        "libcxx": "packages/devel",
        "openssh": "packages/network",
        "openssl": "packages/security",
    }
    for name, group in expected_group.items():
        row = next((row for row in rows if row[0] == name), None)
        if row is None:
            fail(f"{name} is not offered at all")
        if row[4] != group:
            fail(f"{name} is filed under {row[4]} rather than {group}")

    # A program is chosen by whoever configures the build, not by the
    # platform: only the kernel options and the drivers are tied to one.
    for row in rows:
        if row[4].startswith("packages/") or row[4] == "base":
            if row[2] != "*":
                fail(f"{row[0]} is offered only on {row[2]}")

    # The server is built against the library, so one brings the other.
    selected: set = set()
    if menu.package_select(rows, selected, "openssh") is not None:
        fail("a requirement of OpenSSH is not provided")
    if selected != {"openssh", "openssl"}:
        fail(f"choosing OpenSSH selected {sorted(selected)}")
    if not menu.package_dependents(rows, selected, "openssl"):
        fail("OpenSSL can be dropped while OpenSSH still needs it")
    if menu.package_dependents(rows, selected, "openssh"):
        fail("OpenSSH is held by something that does not need it")


def main() -> None:
    expected_targets = {(record[1], record[2]) for record in menu.PLATFORMS}
    if set(menu.BOARD_VARIANTS) != expected_targets:
        fail("board Variant table is incomplete")
    if menu.BOARD_VARIANTS[("amd64", "pcat")] != [
            ("hybrid", "UEFI + BIOS (for PC/AT)"),
            ("uefi", "UEFI (for Apple)"),
            ("bios", "BIOS (for PC/AT)")]:
        fail("amd64 PC/AT Variants changed")

    check_packages()

    template = menu.defaults()

    def fresh_values() -> dict[str, object]:
        result = template.copy()
        result["ZEDBSD_USER_PROGRAMS"] = set(
            template.get("ZEDBSD_USER_PROGRAMS", set()))
        return result

    def round_trip(directory: Path, platform: str, variant: str,
                   suffix: str) -> None:
        values = fresh_values()
        values["ZEDBSD_PLATFORM"] = platform
        values["ZEDBSD_VARIANT"] = variant
        values["CONFIG_BUF_CACHE_KIB"] = "1024"
        path = directory / f"config-{suffix}.mk"
        menu.save(path, values)
        restored = menu.load(path)
        record = menu.platform_record(platform)
        expected = {
            "ZEDBSD_PLATFORM": platform,
            "ZEDBSD_VARIANT": variant,
            "CONFIG_BUF_CACHE_KIB": "1024",
        }
        for key, value in expected.items():
            if str(restored.get(key)) != value:
                fail(f"{suffix} lost {key}={value}")
        text = path.read_text(encoding="utf-8")
        for assignment in [
                f"ZEDBSD_ARCHITECTURE := {record[1]}",
                f"ZEDBSD_BOARD := {record[2]}",
                f"ZEDBSD_VARIANT := {variant}"]:
            if assignment not in text:
                fail(f"{suffix} omitted {assignment}")
        result = make_result(path)
        if result.returncode != 0:
            fail(f"Make rejected valid {suffix}: {result.stdout}")

    def expect_make_rejection(directory: Path, name: str, platform: str,
                              architecture: str, board: str, variant: str,
                              expected: str) -> None:
        path = directory / f"rejected-{name}.mk"
        path.write_text(
            f"ZEDBSD_PLATFORM := {platform}\n"
            f"ZEDBSD_ARCHITECTURE := {architecture}\n"
            f"ZEDBSD_BOARD := {board}\n"
            f"ZEDBSD_VARIANT := {variant}\n",
            encoding="utf-8")
        result = make_result(path)
        if result.returncode == 0 or expected not in result.stdout:
            fail(f"Make accepted invalid {name}: {result.stdout}")

    with tempfile.TemporaryDirectory(prefix="zedbsd-menuconfig-") as temporary:
        directory = Path(temporary)
        for platform, _architecture, _board, _label in menu.PLATFORMS:
            round_trip(directory, platform,
                       menu.variant_default(platform), platform)
        for variant, _variant_label in menu.variants_for_platform("amd64"):
            round_trip(directory, "amd64", variant, f"amd64-{variant}")

        for platform, architecture, board, _label in menu.PLATFORMS:
            old_path = directory / f"old-config-{platform}.mk"
            old_path.write_text(
                "ZEDBSD_MENU_VERSION := 2\n"
                f"ZEDBSD_PLATFORM := {platform}\n"
                f"ZEDBSD_ARCHITECTURE := {architecture}\n"
                f"ZEDBSD_BOARD := {board}\n"
                "ZEDBSD_IMAGE_SIZE_GIB := 256\n"
                "CONFIG_BUF_CACHE_KIB := 4096\n"
                "ZEDBSD_USER_PROGRAMS := ls\n",
                encoding="utf-8")
            restored = menu.load(old_path)
            if (restored["ZEDBSD_VARIANT"] !=
                    menu.variant_default(platform) or
                    restored["CONFIG_BUF_CACHE_KIB"] != "4096" or
                    restored["ZEDBSD_USER_PROGRAMS"] != {"ls"}):
                fail(f"old-config defaults changed unrelated {platform} data")
            migrated_path = directory / f"migrated-config-{platform}.mk"
            menu.save(migrated_path, restored)
            if "ZEDBSD_IMAGE_SIZE_GIB" in migrated_path.read_text(
                    encoding="utf-8"):
                fail(f"obsolete image-size setting survived save for {platform}")
            result = make_result(old_path)
            if result.returncode != 0:
                fail(f"Make rejected old {platform} config: {result.stdout}")

        invalid_path = directory / "invalid-config.mk"
        invalid_path.write_text(
            "ZEDBSD_PLATFORM := amd64\n"
            "ZEDBSD_ARCHITECTURE := amd64\n"
            "ZEDBSD_BOARD := pcat\n"
            "ZEDBSD_VARIANT := broken\n",
            encoding="utf-8")
        restored = menu.load(invalid_path)
        if restored["ZEDBSD_VARIANT"] != "hybrid":
            fail("invalid menu values were not repaired")
        for goals in [[], ["disk-image"], ["build/amd64/hdd-image.img"],
                      ["build/x68k/zedbsd-x68k.hd"]]:
            result = subprocess.run(
                ["make", "--no-print-directory", "-n",
                 f"ZEDBSD_CONFIG={invalid_path}"] + goals,
                cwd=REPO, check=False, text=True,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if (result.returncode == 0 or
                    "Invalid ZEDBSD_VARIANT" not in result.stdout):
                fail(f"image goal accepted invalid config ({goals}): "
                     f"{result.stdout}")

        invalid = [
            ("unknown-variant", "amd64", "amd64", "pcat", "broken",
             "Invalid ZEDBSD_VARIANT"),
            ("pattern-variant", "amd64", "amd64", "pcat", "%",
             "Invalid ZEDBSD_VARIANT"),
            ("multiword-variant", "amd64", "amd64", "pcat", "hybrid bios",
             "Invalid ZEDBSD_VARIANT"),
            ("wrong-board-variant", "i386", "i386", "pcat", "uefi",
             "Invalid ZEDBSD_VARIANT"),
            ("wrong-board", "amd64", "amd64", "rpi4", "default",
             "Invalid target hierarchy"),
            ("wrong-architecture", "amd64", "i386", "pcat", "default",
             "Invalid target hierarchy"),
        ]
        for case in invalid:
            expect_make_rejection(directory, *case)

    print("MAC-T001 menuconfig round-trip: PASS "
          "(6 targets, 3 amd64 Variants, obsolete capacity removed, "
          "package requirements resolved)")


if __name__ == "__main__":
    main()
