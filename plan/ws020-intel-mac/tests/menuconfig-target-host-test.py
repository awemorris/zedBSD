#!/usr/bin/env python3
"""MAC-T001 target Variant/capacity configuration fixture."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import importlib.util
import subprocess
import tempfile
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
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


def main() -> None:
    expected_targets = {(record[1], record[2]) for record in menu.PLATFORMS}
    if set(menu.BOARD_VARIANTS) != expected_targets:
        fail("board Variant table is incomplete")
    if [value for value, _label in menu.IMAGE_SIZE_CHOICES] != [
            "2", "4", "8", "16", "32", "64", "128", "256"]:
        fail("disk image size choices changed")
    if menu.DEFAULT_IMAGE_SIZE_GIB != "2":
        fail("old-config image-size default must remain 2 GiB")
    if menu.BOARD_VARIANTS[("amd64", "pcat")] != [
            ("hybrid", "Hybrid (BIOS+UEFI)"),
            ("bios", "BIOS-only"),
            ("uefi", "UEFI-only (for Apple)")]:
        fail("amd64 PC/AT Variants changed")

    template = menu.defaults()

    def fresh_values() -> dict[str, object]:
        result = template.copy()
        result["ZEDBSD_USER_PROGRAMS"] = set(
            template.get("ZEDBSD_USER_PROGRAMS", set()))
        return result

    def round_trip(directory: Path, platform: str, variant: str,
                   image_size: str, suffix: str) -> None:
        values = fresh_values()
        values["ZEDBSD_PLATFORM"] = platform
        values["ZEDBSD_VARIANT"] = variant
        values["ZEDBSD_IMAGE_SIZE_GIB"] = image_size
        values["CONFIG_BUF_CACHE_KIB"] = "1024"
        path = directory / f"config-{suffix}.mk"
        menu.save(path, values)
        restored = menu.load(path)
        record = menu.platform_record(platform)
        expected = {
            "ZEDBSD_PLATFORM": platform,
            "ZEDBSD_VARIANT": variant,
            "ZEDBSD_IMAGE_SIZE_GIB": image_size,
            "CONFIG_BUF_CACHE_KIB": "1024",
        }
        for key, value in expected.items():
            if str(restored.get(key)) != value:
                fail(f"{suffix} lost {key}={value}")
        text = path.read_text(encoding="utf-8")
        for assignment in [
                f"ZEDBSD_ARCHITECTURE := {record[1]}",
                f"ZEDBSD_BOARD := {record[2]}",
                f"ZEDBSD_VARIANT := {variant}",
                f"ZEDBSD_IMAGE_SIZE_GIB := {image_size}"]:
            if assignment not in text:
                fail(f"{suffix} omitted {assignment}")
        result = make_result(path)
        if result.returncode != 0:
            fail(f"Make rejected valid {suffix}: {result.stdout}")

    def expect_make_rejection(directory: Path, name: str, platform: str,
                              architecture: str, board: str, variant: str,
                              image_size: str, expected: str) -> None:
        path = directory / f"rejected-{name}.mk"
        path.write_text(
            f"ZEDBSD_PLATFORM := {platform}\n"
            f"ZEDBSD_ARCHITECTURE := {architecture}\n"
            f"ZEDBSD_BOARD := {board}\n"
            f"ZEDBSD_VARIANT := {variant}\n"
            f"ZEDBSD_IMAGE_SIZE_GIB := {image_size}\n",
            encoding="utf-8")
        result = make_result(path)
        if result.returncode == 0 or expected not in result.stdout:
            fail(f"Make accepted invalid {name}: {result.stdout}")

    with tempfile.TemporaryDirectory(prefix="zedbsd-menuconfig-") as temporary:
        directory = Path(temporary)
        for platform, _architecture, _board, _label in menu.PLATFORMS:
            for image_size, _size_label in menu.IMAGE_SIZE_CHOICES:
                round_trip(directory, platform,
                           menu.variant_default(platform), image_size,
                           f"{platform}-{image_size}")
        for variant, _variant_label in menu.variants_for_platform("amd64"):
            for image_size, _size_label in menu.IMAGE_SIZE_CHOICES:
                round_trip(directory, "amd64", variant, image_size,
                           f"amd64-{variant}-{image_size}")

        for platform, architecture, board, _label in menu.PLATFORMS:
            old_path = directory / f"old-config-{platform}.mk"
            old_path.write_text(
                "ZEDBSD_MENU_VERSION := 2\n"
                f"ZEDBSD_PLATFORM := {platform}\n"
                f"ZEDBSD_ARCHITECTURE := {architecture}\n"
                f"ZEDBSD_BOARD := {board}\n"
                "CONFIG_BUF_CACHE_KIB := 4096\n"
                "ZEDBSD_USER_PROGRAMS := ls\n",
                encoding="utf-8")
            restored = menu.load(old_path)
            if (restored["ZEDBSD_VARIANT"] !=
                    menu.variant_default(platform) or
                    restored["ZEDBSD_IMAGE_SIZE_GIB"] != "2" or
                    restored["CONFIG_BUF_CACHE_KIB"] != "4096" or
                    restored["ZEDBSD_USER_PROGRAMS"] != {"ls"}):
                fail(f"old-config defaults changed unrelated {platform} data")
            result = make_result(old_path)
            if result.returncode != 0:
                fail(f"Make rejected old {platform} config: {result.stdout}")

        invalid_path = directory / "invalid-config.mk"
        invalid_path.write_text(
            "ZEDBSD_PLATFORM := amd64\n"
            "ZEDBSD_ARCHITECTURE := amd64\n"
            "ZEDBSD_BOARD := pcat\n"
            "ZEDBSD_VARIANT := broken\n"
            "ZEDBSD_IMAGE_SIZE_GIB := 3\n",
            encoding="utf-8")
        restored = menu.load(invalid_path)
        if (restored["ZEDBSD_VARIANT"] != "hybrid" or
                restored["ZEDBSD_IMAGE_SIZE_GIB"] != "2"):
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
            ("unknown-variant", "amd64", "amd64", "pcat", "broken", "2",
             "Invalid ZEDBSD_VARIANT"),
            ("pattern-variant", "amd64", "amd64", "pcat", "%", "2",
             "Invalid ZEDBSD_VARIANT"),
            ("multiword-variant", "amd64", "amd64", "pcat", "hybrid bios",
             "2", "Invalid ZEDBSD_VARIANT"),
            ("wrong-board-variant", "i386", "i386", "pcat", "uefi", "2",
             "Invalid ZEDBSD_VARIANT"),
            ("wrong-board", "amd64", "amd64", "rpi4", "default", "2",
             "Invalid target hierarchy"),
            ("wrong-architecture", "amd64", "i386", "pcat", "default", "2",
             "Invalid target hierarchy"),
        ]
        invalid.extend(
            (name, "amd64", "amd64", "pcat", "hybrid", image_size,
             "Invalid ZEDBSD_IMAGE_SIZE_GIB")
            for name, image_size in [
                ("size-0", "0"), ("size-3", "3"),
                ("size-257", "257"), ("size-text", "large"),
                ("size-pattern", "%"), ("size-multiword", "2 4"),
                ("size-empty", "")])
        for case in invalid:
            expect_make_rejection(directory, *case)

    print("MAC-T001 menuconfig round-trip: PASS "
          "(6 targets, 8 capacities, 3 amd64 Variants)")


if __name__ == "__main__":
    main()
