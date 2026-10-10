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
    for _label, group in menu.package_categories(rows):
        offered.update(row[0] for row in rows if row[4] == group)
    for row in rows:
        if row[4].startswith("packages/") and row[0] not in offered:
            fail(f"{row[0]} is filed under {row[4]}, which the menu omits")

    # Where each package is filed, and that the menu reaches it there.
    expected_group = {
        "noct": "base",
        "emacs": "base",
        "libcxx": "packages/devel",
        "openssh": "packages/network",
        "openssl": "packages/security",
        "noto-color-emoji": "packages/fonts",
    }
    for name, group in expected_group.items():
        row = next((row for row in rows if row[0] == name), None)
        if row is None:
            fail(f"{name} is not offered at all")
        if row[4] != group:
            fail(f"{name} is filed under {row[4]} rather than {group}")

    # 2026-10-10 user: Base, Desktop, Packages and Firmware entries are
    # selectable on every CPU. The direct-config-only groups stay separate.
    for row in rows:
        if row[4].startswith("packages/") or row[4] in ("base", "comp", "desktop", "firmware"):
            if row[2] != "*":
                fail(f"{row[0]} is offered only on {row[2]}")

    # Noct is a base program beside Emacs, which is written in it, and the
    # Packages menu no longer offers it (2026-10-09 user: "Noctは
    # userland/base/noct/にあるけど、Baseメニューにないようなので、追加して
    # ください。", "PackagesメニューからNoctを削除してください。").
    for _label, group in menu.package_categories(rows):
        for row in rows:
            if row[4] == group and (row[0] == "noct" or row[5] == "base/noct"):
                fail(f"the Packages menu ({group}) still offers Noct as {row[0]}")
    base_values = {"ZEDBSD_PLATFORM": "amd64", "ZEDBSD_USER_PROGRAMS": set()}
    base_names = [row[0] for row in menu.group_rows(base_values, menu.BASE_GROUPS)]
    for name in ("noct", "emacs"):
        if name not in base_names:
            fail(f"Base > Select does not offer {name} on amd64")
    selected = set()
    if menu.package_select(rows, selected, "emacs") is not None:
        fail("a requirement of Emacs is not provided")
    if selected != {"emacs", "noct"}:
        fail(f"choosing Emacs selected {sorted(selected)}")
    if not menu.package_dependents(rows, selected, "noct"):
        fail("Noct can be dropped while Emacs still needs it")

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


def check_fonts_menu() -> None:
    """Walks Packages > Fonts the way a user would and chooses the emoji font.

    The curses screen is left out: choose() is replaced by answers given in
    order, each checked against the labels the menu shows (BUG-129).
    """
    shown: list[tuple[str, list[str]]] = []
    emoji = next((row for row in USER_PROGRAM_ROWS if row[0] == "noto-color-emoji"), None)
    if emoji is None:
        fail("noto-color-emoji is not offered at all")

    def answer(title: str, labels: list[str]) -> int | None:
        if title == "Packages" and len([t for t, _l in shown if t == "Packages"]) == 1:
            if "Fonts" not in labels:
                fail(f"the Packages menu shows no Fonts: {labels}")
            return labels.index("Fonts")
        if title == "Fonts":
            entries = [label for label in labels if label[4:] == emoji[1]]
            if not entries:
                fail(f"the Fonts menu does not offer noto-color-emoji: {labels}")
            if len([t for t, _l in shown if t == "Fonts"]) == 1:
                return labels.index(entries[0])
            if not entries[0].startswith("[*]"):
                fail(f"noto-color-emoji is not marked chosen: {entries[0]}")
            return len(labels) - 1
        return len(labels) - 1

    def choose(_screen, title, labels, _target, _selected=0):
        shown.append((title, list(labels)))
        if len(shown) > 10:
            fail("the Fonts walk did not end")
        return answer(title, labels)

    real_choose = menu.choose
    menu.choose = choose
    values: dict[str, object] = {"ZEDBSD_PLATFORM": "amd64", "ZEDBSD_USER_PROGRAMS": set()}
    try:
        menu.select_package_programs(None, values)
    finally:
        menu.choose = real_choose
    if "noto-color-emoji" not in values["ZEDBSD_USER_PROGRAMS"]:
        fail("choosing it from Packages > Fonts did not select noto-color-emoji")


def scratch_directory(name: str) -> Path:
    """A new directory under build/tmp for one check (left there: removing is the main session's step)."""
    parent = REPO / "build" / "tmp"
    parent.mkdir(parents=True, exist_ok=True)
    return Path(tempfile.mkdtemp(prefix=f"menuconfig-{name}.", dir=parent))


def check_menu_layout() -> None:
    """The menu the 2026-10-09 user decisions laid out (WS193): the main menu's
    entries, the Boot Option toggles, Base's All, a value the menu no longer
    shows kept through a save, and the build's progress reading make's trace."""
    shown: list[tuple[str, list[str]]] = []

    def choose(_screen, title, labels, _target, _selected=0):
        shown.append((title, list(labels)))
        return labels.index("Exit") if "Exit" in labels else None

    real_choose, real_save, real_cursor = menu.choose, menu.save, menu.curses.curs_set
    menu.choose = choose
    menu.save = lambda _path, _values: None
    menu.curses.curs_set = lambda _visibility: None
    try:
        menu.tui(_FakeScreen(), menu.defaults(), Path("/nonexistent"))
    finally:
        menu.choose, menu.save, menu.curses.curs_set = real_choose, real_save, real_cursor
    expected = ["CPU / Board", "Boot Option", "Drivers", "Development", "Base", "Desktop",
                "Packages", "Firmware", "", "Build boot image", "", "Exit"]
    if not shown or shown[0][1] != expected:
        fail(f"the main menu is {shown[0][1] if shown else 'not shown'}, not {expected}")

    values = menu.defaults()
    answers = iter([0, 0, 1, 3])

    def boot_choose(_screen, _title, _labels, _target, _selected=0):
        return next(answers)

    menu.choose = boot_choose
    try:
        menu.boot_options(None, values)
    finally:
        menu.choose = real_choose
    if (values["ZEDBSD_GRAPHICAL_BOOT"], values["ZEDBSD_BOOT_KERNEL_MESSAGES"],
            values["ZEDBSD_GRAPHICAL_LOGIN"]) != ("y", "n", "n"):
        fail(f"Boot Option toggles left {values['ZEDBSD_GRAPHICAL_BOOT']}/"
             f"{values['ZEDBSD_BOOT_KERNEL_MESSAGES']}/{values['ZEDBSD_GRAPHICAL_LOGIN']}")

    values = menu.defaults()
    base = {row[0] for row in menu.group_rows(values, menu.BASE_GROUPS)}
    menu.toggle_all(values, menu.BASE_GROUPS)
    if not base <= values["ZEDBSD_USER_PROGRAMS"] or not menu.all_selected(values, menu.BASE_GROUPS):
        fail("Base > All did not select every base program")
    menu.toggle_all(values, menu.BASE_GROUPS)
    defaults = {row[0] for row in menu.group_rows(values, menu.BASE_GROUPS) if row[3] == "y"}
    if values["ZEDBSD_USER_PROGRAMS"] & base != defaults:
        fail("Base > All a second time did not go back to the defaults")

    path = scratch_directory("layout") / "config.mk"
    values = menu.defaults()
    values["ZEDBSD_USER_PROGRAMS"].add("zterm")
    values["CONFIG_BUF_CACHE_KIB"] = "4096"
    menu.save(path, values)
    kept = menu.load(path)
    if "zterm" not in kept["ZEDBSD_USER_PROGRAMS"] or kept["CONFIG_BUF_CACHE_KIB"] != "4096":
        fail("a selection the menu does not show (X11, the kernel options) was not kept")

    if menu.trace_target("Makefile:12: update target 'build/amd64/vmunix' due to: x") != "build/amd64/vmunix":
        fail("the build's progress does not read make's trace")
    if menu.trace_target("cc -c x.c") is not None:
        fail("the build's progress counts a line that is no target")


def check_build_progress() -> None:
    """Build boot image on a stand-in tree whose disk-image has three steps:
    the progress counts the targets make's trace names, and the result says
    the build succeeded (and, for a failing tree, shows the log's end)."""
    drawn: list[tuple[int, int, str]] = []
    results: list[list[str]] = []
    real = (menu.REPO, menu.choose, menu.save, menu.draw_progress, menu.message)
    menu.choose = lambda _screen, _title, _labels, _target, _selected=0: 0
    menu.save = lambda _path, _values: None
    menu.draw_progress = lambda _screen, _values, done, total, current: drawn.append((done, total, current))
    menu.message = lambda _screen, _title, lines, _target: results.append(list(lines))
    try:
        tree = scratch_directory("build")
        menu.REPO = tree
        (tree / "Makefile").write_text(
            "disk-image: first second\n\t@true\nfirst:\n\t@true\nsecond:\n\t@true\n"
            "broken:\n", encoding="utf-8")
        values = {"ZEDBSD_PLATFORM": "amd64"}
        menu.build_boot_image(None, values, tree / "config.mk")
        if not results or results[-1][0] != "Build succeeded.":
            fail(f"the stand-in build did not succeed: {results}")
        counted = [entry for entry in drawn if entry[2] in ("first", "second", "disk-image")]
        if len(counted) != 3 or counted[-1][0] != 3 or counted[-1][1] != 3:
            fail(f"the progress counted {drawn}")
        (tree / "Makefile").write_text("disk-image:\n\t@echo the step that failed; false\n", encoding="utf-8")
        menu.build_boot_image(None, values, tree / "config.mk")
        if not results[-1][0].startswith("Build failed") or "the step that failed" not in results[-1]:
            fail(f"a failed build did not show the log's end: {results[-1]}")
    finally:
        menu.REPO, menu.choose, menu.save, menu.draw_progress, menu.message = real


class _FakeScreen:
    """A screen the main menu can be opened on without a terminal."""

    def keypad(self, _flag):
        return None


def main() -> None:
    expected_targets = {(record[1], record[2]) for record in menu.PLATFORMS}
    if set(menu.BOARD_VARIANTS) != expected_targets:
        fail("board Variant table is incomplete")
    if menu.BOARD_VARIANTS[("amd64", "pcat")] != [
            ("native", "UEFI, UFS root partition (for PC/AT)"),
            ("hybrid", "UEFI + BIOS (for PC/AT)"),
            ("uefi", "UEFI (for Apple)"),
            ("bios", "BIOS (for PC/AT)")]:
        fail("amd64 PC/AT Variants changed")

    check_packages()
    check_fonts_menu()
    check_menu_layout()
    check_build_progress()

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
        values["ZEDBSD_GRAPHICAL_LOGIN"] = "n"
        path = directory / f"config-{suffix}.mk"
        menu.save(path, values)
        restored = menu.load(path)
        record = menu.platform_record(platform)
        expected = {
            "ZEDBSD_PLATFORM": platform,
            "ZEDBSD_VARIANT": variant,
            "CONFIG_BUF_CACHE_KIB": "1024",
            "ZEDBSD_GRAPHICAL_LOGIN": "n",
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
        if restored["ZEDBSD_VARIANT"] != "native":
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
          "package requirements resolved, the WS193 menu)")


if __name__ == "__main__":
    main()
