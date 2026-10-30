#!/usr/bin/env python3
"""Curses build menu and configuration editor for zedBSD."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import curses
import os
import re
import subprocess
import textwrap
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
CONFIG_DIR = REPO / "config"

PLATFORMS = [
    ("i386", "i386", "pcat", "i386 PC/AT compatible"),
    ("amd64", "amd64", "pcat", "x86_64 PC/AT compatible"),
    ("pc98", "i386", "pc98", "NEC PC-9800"),
    ("rpi4", "arm64", "rpi4", "Raspberry Pi 4 Arm64"),
    ("odroid-c5", "arm64", "odroid-c5", "Hardkernel ODROID-C5 (Amlogic S905X5M)"),
    ("riscv64-virt", "riscv64", "virt", "QEMU virt RISC-V 64"),
    ("visionfive2", "riscv64", "visionfive2", "StarFive VisionFive 2"),
    ("riscv32-virt", "riscv32", "virt", "QEMU virt RISC-V 32"),
    ("armv7-virt", "armv7", "virt", "QEMU virt ARMv7-A"),
    ("zybo", "armv7", "zybo", "Digilent ZYBO (Zynq-7010)"),
    ("sun4u", "sparcv9", "sun4u", "SPARC V9 sun4u"),
    ("x68k", "m68k", "x68k", "Sharp X68000 MC68030"),
    ("m68k-virt", "m68k", "virt", "QEMU virt MC68040"),
    ("ppc32-mac99", "ppc32", "mac99", "QEMU mac99 PowerPC 32"),
    ("ppc64-pseries", "ppc64", "pseries", "QEMU pseries PowerPC 64"),
    ("mips32-malta", "mips32", "malta", "QEMU malta MIPS32 little-endian"),
    ("mips64-malta", "mips64", "malta", "QEMU malta MIPS64 little-endian"),
    ("mips32-rg350", "mips32", "rg350", "Anbernic RG350 (Ingenic JZ4770)"),
]

BOARD_LABELS = {
    "pcat": "IBM PC/AT compatible",
    "pc98": "NEC PC-9800",
    "rpi4": "Raspberry Pi 4",
    "odroid-c5": "Hardkernel ODROID-C5",
    "virt": "QEMU virt",
    "zybo": "Digilent ZYBO",
    "visionfive2": "StarFive VisionFive 2",
    "sun4u": "sun4u",
    "x68k": "Sharp X68000",
    "mac99": "QEMU mac99",
    "pseries": "QEMU pseries",
    "malta": "QEMU malta",
    "rg350": "Anbernic RG350",
}

# Variant is a board-owned image-layout choice.  It deliberately does not
# participate in platform/source selection: the platform record above remains
# the sole Architecture + Board build target.
BOARD_VARIANTS = {
    ("i386", "pcat"): [("default", "Default")],
    ("amd64", "pcat"): [
        ("native", "UEFI, UFS root partition (for PC/AT)"),
        ("hybrid", "UEFI + BIOS (for PC/AT)"),
        ("uefi", "UEFI (for Apple)"),
        ("bios", "BIOS (for PC/AT)"),
    ],
    ("i386", "pc98"): [("default", "Default")],
    ("arm64", "rpi4"): [("default", "Default")],
    ("arm64", "odroid-c5"): [("default", "Default")],
    ("riscv64", "virt"): [("default", "Default")],
    ("riscv64", "visionfive2"): [("default", "Default")],
    ("riscv32", "virt"): [("default", "Default")],
    ("armv7", "virt"): [("default", "Default")],
    ("armv7", "zybo"): [("default", "Default")],
    ("sparcv9", "sun4u"): [("default", "Default")],
    ("m68k", "x68k"): [("default", "Default")],
    ("m68k", "virt"): [("default", "Default")],
    ("ppc32", "mac99"): [("default", "Default")],
    ("ppc64", "pseries"): [("default", "Default")],
    ("mips32", "malta"): [("default", "Default")],
    ("mips64", "malta"): [("default", "Default")],
    ("mips32", "rg350"): [("default", "Default")],
}

# The names the Packages menu gives the directories of userland/packages
# (WS193: the menu is built from the directories the packages are in, so a
# new directory needs no change here; one without a name here is shown
# capitalized).
PACKAGE_LABELS = {
    "lang": "Languages",
    "devel": "Development",
    "libs": "Libraries",
    "desktop": "Desktop",
    "multimedia": "Multimedia",
    "fonts": "Fonts",
    "network": "Network",
    "security": "Security",
}

# The CPUs and the boards each one boots on (2026-10-09 user, WS193; i386
# with the PC-9800 and the PC/AT, 2026-10-14 user, WS232): the platform each
# pair builds.  Choosing a CPU selects its first board; a CPU with more than
# one board offers them under Board.  Another platform read from a config.mk
# is kept as it is and shown as the current one.
MENU_CPUS = [
    ("amd64", "x86_64", "amd64", "UEFI + ACPI"),
    ("arm64", "arm64", "rpi4", "Raspberry Pi 4"),
    ("arm64", "arm64", "odroid-c5", "ODROID-C5"),
    ("i386", "i386", "pc98", "PC-9800"),
    ("i386", "i386", "i386", "PC/AT"),
    ("riscv64", "RISC-V 64", "riscv64-virt", "QEMU virt"),
    ("riscv64", "RISC-V 64", "visionfive2", "StarFive VisionFive 2"),
    ("riscv32", "RISC-V 32", "riscv32-virt", "QEMU virt"),
]

# The user programs' groups the Base, Desktop and Firmware menus select
# from.  The test programs, the X11 programs other than Retro's, the disk
# layout (Variant), the kernel's options, the test hooks and Noct's GPU
# accelerator are not in the menu (2026-10-09 user: "menu から外す",
# "X11とTestsはメニューから削除し、直接記述のみにする"): they are set in
# config.mk by hand, and the values a config.mk holds are read and written
# back as they are.
BASE_GROUPS = ("base", "comp")
DESKTOP_GROUPS = ("desktop",)
FIRMWARE_GROUPS = ("firmware",)

# The Retro menu, on every CPU (2026-10-14 user, WS232: "アーキにかかわらず
# menuconfigのトップにRetroというメニューを追加して、Xzed, zwm, zshell, zterm
# を追加してほしいです。"), the main menu's entry right after Base (2026-10-14
# user: "Retroが一番上に来てます。Baseの下にしてください。", "全部の CPU で
# Base の下"): these X11 programs by name, in this order, each shown as its
# command's name.
RETRO_PROGRAMS = ("Xzed", "zwm", "zshell", "zterm")

# The CPUs the Desktop (Keiland, Vulkan, Wayland) is not built for
# (2026-10-14 user, WS232: "i386 では Desktop を自動で外す"): on them the
# menu offers no Desktop and writes none of its programs, nor a program that
# needs one of them, as config/ci/config-pc98.mk has only Retro and Base.
# As with any selection that does not build for the target, the menu keeps
# the Desktop's selection while i386 is chosen, so switching back to x86_64
# or arm64 in the same session writes it again.  RISC-V 64 is treated the
# same way: its port builds the base system only.
NO_DESKTOP_ARCHITECTURES = ("i386", "riscv64", "riscv32", "mips32", "mips64", "armv7")


def package_categories(rows: list[list[str]]) -> list[tuple[str, str]]:
    """The Packages menu's categories: the directories of userland/packages
    that hold a package, in order, each with its name."""
    groups = sorted({row[4] for row in rows if row[4].startswith("packages/")})
    return [(PACKAGE_LABELS.get(group.split("/", 1)[1],
                                group.split("/", 1)[1].capitalize()), group)
            for group in groups]


def platform_record(name: str):
    return next((item for item in PLATFORMS if item[0] == name), PLATFORMS[0])


def variants_for_platform(platform: str) -> list[tuple[str, str]]:
    _name, architecture, board, _label = platform_record(platform)
    return BOARD_VARIANTS[(architecture, board)]


def variant_default(platform: str) -> str:
    return variants_for_platform(platform)[0][0]


def applies(specification: str, platform: str) -> bool:
    return specification == "*" or platform in specification.replace(",", " ").split()


def desktop_builds(platform: str) -> bool:
    """Whether the Desktop is built for this platform."""
    return platform_record(platform)[1] not in NO_DESKTOP_ARCHITECTURES


def builds_for(platform: str) -> set[str]:
    """The user programs that build for this platform: those whose platforms
    name it, without, where the Desktop is not built, its programs and the
    programs that need them, directly or through one another."""
    rows = user_program_rows()
    result = {row[0] for row in rows if applies(row[2], platform)}
    if desktop_builds(platform):
        return result
    missing = {row[5] for row in rows if row[4] in DESKTOP_GROUPS and row[5]}
    result.difference_update(row[0] for row in rows if row[4] in DESKTOP_GROUPS)
    while True:
        needing = {row[0] for row in rows
                   if row[0] in result and missing.intersection(row[6].split())}
        if not needing:
            return result
        result.difference_update(needing)
        missing.update(row[5] for row in rows if row[0] in needing and row[5])


def read_rows(path: Path, fields: int) -> list[list[str]]:
    result = []
    if not path.exists():
        return result
    for number, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = [part.strip() for part in line.split("|")]
        if len(parts) != fields:
            raise SystemExit(f"{path}:{number}: expected {fields} fields")
        result.append(parts)
    return result


def all_option_files() -> list[Path]:
    result = [CONFIG_DIR / "kernel-options.list",
              CONFIG_DIR / "rootfs-options.list",
              CONFIG_DIR / "drivers" / "isa.drivers",
              CONFIG_DIR / "drivers" / "pci.drivers",
              CONFIG_DIR / "drivers" / "usb.drivers",
              CONFIG_DIR / "drivers" / "generic.drivers"]
    result.extend(architecture_files())
    return result


def driver_rows() -> list[list[str]]:
    """One row per configurable driver, independent of the selected CPU."""
    rows = {}
    for path in all_option_files():
        for row in read_rows(path, 6):
            if row[0].startswith("CONFIG_DRIVER_") and row[1] == "bool":
                rows.setdefault(row[0], row)
    return list(rows.values())


def driver_defaults(platform: str) -> dict[str, str]:
    """Platform defaults apply only to options a configuration has not set."""
    values = {row[0]: "n" for row in driver_rows()}
    for path in all_option_files():
        for key, kind, _label, targets, default, _choices in read_rows(path, 6):
            if key in values and kind == "bool" and applies(targets, platform):
                values[key] = default
    return values


def architecture_files() -> list[Path]:
    return sorted((CONFIG_DIR / "drivers" / "architecture").glob("*.drivers"))


def platform_driver_rows(platform: str) -> list[list[str]]:
    """Drivers > Platform on a platform: the rows of the architecture
    catalogs that name it, the always-built (fixed) ones too, in the
    catalogs' order."""
    rows = []
    for path in architecture_files():
        for row in read_rows(path, 6):
            if row[3] != "*" and applies(row[3], platform) and \
                    (row[1] == "fixed" or row[0].startswith("CONFIG_DRIVER_")):
                rows.append(row)
    return rows


def driver_firmware_table() -> dict[str, list[str]]:
    """config/drivers/firmware.list: the firmware packages each driver needs."""
    return {key: packages.split()
            for key, packages in read_rows(CONFIG_DIR / "drivers" / "firmware.list", 2)}


def driver_firmware(values: dict[str, object], platform: str) -> set[str]:
    """The firmware packages the drivers that are on and build for this
    platform need, and the ones the board itself needs (a row whose key is
    the platform's name, such as its bootloader)."""
    targets = {row[0]: row[3] for row in driver_rows()}
    needed: set[str] = set()
    for key, packages in driver_firmware_table().items():
        if key == platform or (values.get(key) == "y" and
                               applies(targets.get(key, ""), platform)):
            needed.update(packages)
    return needed


def follow_driver_firmware(values: dict[str, object], before: set[str]) -> None:
    """Brings the Firmware selection in line with the drivers after a change
    (2026-10-14 user, WS232 p004): a package the drivers needed before and
    no longer need is taken out, and every package they need now is in.  A
    package chosen in the Firmware menu that no driver needed is kept."""
    after = driver_firmware(values, str(values["ZEDBSD_PLATFORM"]))
    programs = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
    programs.difference_update(before - after)
    programs.update(after)


def choose_platform(values: dict[str, object], platform: str) -> None:
    """Makes the menu's target this platform.  Its Platform drivers take
    their catalog defaults: the menu shows them on this platform alone, so
    a value held from another CPU was never chosen for this board (a
    Raspberry Pi 4 chosen from the x86_64 default used to keep its onboard
    Ethernet, Wi-Fi, Bluetooth, audio and GPU off).  The firmware follows
    the drivers of the new platform."""
    before = driver_firmware(values, str(values.get("ZEDBSD_PLATFORM", "amd64")))
    if values.get("ZEDBSD_PLATFORM") != platform:
        for row in platform_driver_rows(platform):
            if row[1] == "bool":
                values[row[0]] = row[4]
    values["ZEDBSD_PLATFORM"] = platform
    normalize(values)
    follow_driver_firmware(values, before)


def driver_groups(platform: str) -> list[tuple[str, list[list[str]]]]:
    """The functional categories on a platform, each containing its driver
    rows.  config/drivers/menu.list places every bus driver, the same on
    every CPU; Platform holds the architecture catalogs' drivers of this
    platform alone (2026-10-14 user, WS232 p004)."""
    board = {row[0] for path in architecture_files()
             for row in read_rows(path, 6) if row[0] != "-"}
    catalog = {row[0]: row for row in driver_rows()}
    groups: list[tuple[str, list[list[str]]]] = []
    assigned = set()
    for title, keys in read_rows(CONFIG_DIR / "drivers" / "menu.list", 2):
        rows = []
        for key in keys.split():
            if key in board:
                raise SystemExit(f"config/drivers/menu.list: {key} is a Platform driver")
            if key in assigned or key not in catalog:
                raise SystemExit(f"config/drivers/menu.list: {key} is unknown or listed twice")
            assigned.add(key)
            rows.append(catalog[key])
        if title == "Platform":
            rows.extend(platform_driver_rows(platform))
        groups.append((title, rows))
    missing = sorted(set(catalog) - board - assigned)
    if missing:
        raise SystemExit(f"config/drivers/menu.list: no category for {' '.join(missing)}")
    return [(title, rows) for title, rows in groups if rows]


def defaults() -> dict[str, object]:
    # amd64 is the default target (2026-09-29 user decision): the default image
    # is the one that starts the desktop.
    values: dict[str, object] = {
        "ZEDBSD_PLATFORM": "amd64",
        "ZEDBSD_VARIANT": variant_default("amd64"),
    }
    for path in all_option_files():
        for key, kind, _label, _targets, default, _choices in read_rows(path, 6):
            if key != "-" and kind != "fixed" and key not in values:
                values[key] = default
    values.update(driver_defaults(str(values["ZEDBSD_PLATFORM"])))
    programs = user_program_rows()
    values["ZEDBSD_USER_PROGRAMS"] = {
        row[0] for row in programs if row[3] == "y"
    }
    follow_driver_firmware(values, set())
    return values


def load(path: Path) -> dict[str, object]:
    values = defaults()
    if not path.exists():
        return values
    configured = set()
    pattern = re.compile(r"^([A-Z][A-Z0-9_]*)\s*(?::=|=)\s*(.*?)\s*$")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        key, value = match.groups()
        configured.add(key)
        if key == "ZEDBSD_USER_PROGRAMS":
            values[key] = set(value.split())
        else:
            values[key] = value
    if str(values.get("ZEDBSD_MENU_VERSION", "1")) == "1":
        selected = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
        for row in user_program_rows():
            if row[3] == "y" and row[4].startswith("packages/"):
                selected.add(row[0])
    if str(values.get("ZEDBSD_PLATFORM")) not in {item[0] for item in PLATFORMS}:
        values["ZEDBSD_PLATFORM"] = "amd64"
    for key, value in driver_defaults(str(values["ZEDBSD_PLATFORM"])).items():
        if key not in configured:
            values[key] = value
    normalize_target(values)
    return values


def normalize_target(values: dict[str, object]) -> None:
    platform = str(values.get("ZEDBSD_PLATFORM", "amd64"))
    if platform not in {item[0] for item in PLATFORMS}:
        platform = "amd64"
        values["ZEDBSD_PLATFORM"] = platform
    allowed_variants = {value for value, _label in variants_for_platform(platform)}
    if str(values.get("ZEDBSD_VARIANT", "")) not in allowed_variants:
        values["ZEDBSD_VARIANT"] = variant_default(platform)


def normalize(values: dict[str, object]) -> None:
    normalize_target(values)
    platform = str(values["ZEDBSD_PLATFORM"])
    # Driver choices survive saving and CPU changes, including foreign drivers.
    if platform != "amd64":
        values["CONFIG_KERNEL_TEST_CHECKPOINTS"] = "n"
    available_programs = {row[0] for row in user_program_rows()}
    selected_programs = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
    selected_programs.intersection_update(available_programs)
    if "Xzed" in selected_programs:
        values["CONFIG_DRIVER_GRAPHICS"] = "y"


def save(path: Path, values: dict[str, object]) -> None:
    normalize(values)
    platform, architecture, board, _label = platform_record(
        str(values["ZEDBSD_PLATFORM"]))
    lines = [
        "# Automatically generated by the zedBSD build menu.  Do not edit.",
        "ZEDBSD_MENU_VERSION := 3",
        f"ZEDBSD_PLATFORM := {platform}",
        f"ZEDBSD_ARCHITECTURE := {architecture}",
        f"ZEDBSD_BOARD := {board}",
        f"ZEDBSD_VARIANT := {values['ZEDBSD_VARIANT']}",
        "",
    ]
    emitted = set()
    for option_file in all_option_files():
        for key, kind, _label, _targets, _default, _choices in read_rows(option_file, 6):
            if key == "-" or kind == "fixed" or key in emitted:
                continue
            lines.append(f"{key} := {values.get(key, 'n')}")
            emitted.add(key)
    # A selection that does not build for this platform stays in the menu's
    # state, so switching the target back keeps it, but is not written.
    programs = values.get("ZEDBSD_USER_PROGRAMS", set())
    building = builds_for(platform)
    ordered = [row[0] for row in user_program_rows()
               if row[0] in programs and row[0] in building]
    lines.extend(["", "ZEDBSD_USER_PROGRAMS := " + " ".join(ordered)])
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def addstr(window, y: int, x: int, text: str, width: int,
           attr: int = curses.A_NORMAL) -> None:
    if width <= 0:
        return
    try:
        window.addnstr(y, x, text, width, attr)
    except curses.error:
        pass


def draw_title(screen, title: str, target: str) -> None:
    height, width = screen.getmaxyx()
    heading = f" zedBSD Build Menu — Current target: {target} "
    addstr(screen, 0, max(0, (width - len(heading)) // 2), heading,
           width, curses.A_REVERSE | curses.A_BOLD)
    addstr(screen, 2, 2, title, width - 4, curses.A_BOLD)
    if height > 4:
        addstr(screen, height - 2, 2,
               "Arrow keys navigate; Enter selects; Q/Esc goes back.",
               width - 4, curses.A_REVERSE)


def choose(screen, title: str, labels: list[str], target: str,
           selected: int = 0) -> int | None:
    selectable = [index for index, label in enumerate(labels) if label]
    if not selectable:
        message(screen, title, ["No entries are available for this target."], target)
        return None
    offset = 0
    selected = max(0, min(selected, len(labels) - 1))
    if selected not in selectable:
        selected = min(selectable, key=lambda index: abs(index - selected))
    while True:
        screen.erase()
        height, width = screen.getmaxyx()
        draw_title(screen, title, target)
        visible = max(1, height - 7)
        if selected < offset:
            offset = selected
        elif selected >= offset + visible:
            offset = selected - visible + 1
        for row, label in enumerate(labels[offset:offset + visible]):
            index = offset + row
            attr = curses.A_REVERSE if index == selected else curses.A_NORMAL
            addstr(screen, 4 + row, 4, label, width - 8, attr)
        screen.refresh()
        key = screen.getch()
        if key in (curses.KEY_UP, ord("k")):
            position = selectable.index(selected)
            selected = selectable[(position - 1) % len(selectable)]
        elif key in (curses.KEY_DOWN, ord("j")):
            position = selectable.index(selected)
            selected = selectable[(position + 1) % len(selectable)]
        elif key in (10, 13, curses.KEY_RIGHT, ord(" ")):
            return selected
        elif key in (27, ord("q"), ord("Q"), curses.KEY_LEFT):
            return None


def message(screen, title: str, lines: list[str], target: str) -> None:
    screen.erase()
    height, width = screen.getmaxyx()
    draw_title(screen, title, target)
    row = 4
    for line in lines:
        if row >= height - 3:
            break
        addstr(screen, row, 4, line, width - 8)
        row += 1
    addstr(screen, height - 3, 4, "Press Enter to continue.", width - 8,
           curses.A_BOLD)
    screen.refresh()
    while screen.getch() not in (10, 13, 27, ord("q"), ord("Q")):
        pass


def target_label(values: dict[str, object]) -> str:
    return platform_record(str(values["ZEDBSD_PLATFORM"]))[3]


def package_select(rows: list[list[str]], selected: set, name: str):
    """Selects a program and whatever it needs.

    Returns the name of a requirement that no program provides, having
    selected nothing; returns None when everything asked for was found.
    A requirement of a requirement is followed as well, so that choosing
    one program brings in the whole of what it is built against.
    """
    added = {name}
    pending = next((row[6].split() for row in rows if row[0] == name), [])
    while pending:
        requirement = pending.pop(0)
        dependency = next((row for row in rows if row[5] == requirement), None)
        if dependency is None:
            return requirement
        if dependency[0] not in selected and dependency[0] not in added:
            added.add(dependency[0])
            pending.extend(dependency[6].split())
    selected.update(added)
    return None


def package_dependents(rows: list[list[str]], selected: set,
                       name: str) -> list[str]:
    """Returns the labels of the selected programs that need this one."""
    package = next((row[5] for row in rows if row[0] == name), "")
    if not package:
        return []
    return [row[1] for row in rows
            if row[0] in selected and row[0] != name and
            package in row[6].split()]


def in_menu(program: str, group: str) -> bool:
    """Whether the menu shows this program of this group (the test programs
    and the X11 programs other than Retro's are only set in config.mk by
    hand)."""
    return (program in RETRO_PROGRAMS or
            group in BASE_GROUPS + DESKTOP_GROUPS + FIRMWARE_GROUPS or
            group.startswith("packages/"))


def removal_closure(rows: list[list[str]], selected: set,
                    name: str) -> tuple[list[str], list[str]]:
    """What turning a program off takes with it.  Returns the labels of the
    shown programs that still need it (which block the removal) and the
    names of the programs outside the menu that need it, directly or
    through one another, and go with it; a program the menu does not show
    cannot be turned off by hand, so it must not keep a shown one on."""
    groups = {row[0]: row[4] for row in rows}
    labels = {row[0]: row[1] for row in rows}
    packages = {row[0]: row[5] for row in rows}
    needs = {row[0]: row[6].split() for row in rows}
    hidden = []
    removing = {name}
    pending = [name]
    while pending:
        package = packages.get(pending.pop(), "")
        if not package:
            continue
        for program in sorted(selected):
            if (program not in removing and package in needs.get(program, []) and
                    not in_menu(program, groups.get(program, ""))):
                removing.add(program)
                hidden.append(program)
                pending.append(program)
    gone = {packages[program] for program in removing if packages.get(program)}
    blocking = [labels[program] for program in selected
                if program not in removing and in_menu(program, groups.get(program, "")) and
                gone.intersection(needs.get(program, []))]
    return sorted(blocking), hidden


def edit_program_group(screen, values: dict[str, object], groups: tuple[str, ...],
                       title: str) -> None:
    edit_programs(screen, values, group_rows(values, groups), title)


def retro_rows(values: dict[str, object]) -> list[list[str]]:
    """Retro's programs that build for the current platform, in Retro's order."""
    building = builds_for(str(values["ZEDBSD_PLATFORM"]))
    rows = {row[0]: row for row in user_program_rows()}
    return [rows[name] for name in RETRO_PROGRAMS
            if name in rows and name in building]


def edit_programs(screen, values: dict[str, object], rows: list[list[str]],
                  title: str) -> None:
    """Turns the programs of one menu on and off, one by one."""
    selected_programs = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
    selected = 0
    while True:
        labels = [("[*]" if name in selected_programs else "[ ]") + " " + label
                  for name, label, *_rest in rows]
        labels.append("Back")
        choice = choose(screen, title, labels,
                        target_label(values), selected)
        if choice is None or choice == len(rows):
            return
        selected = choice
        name = rows[choice][0]
        if name in selected_programs:
            dependents, hidden = removal_closure(user_program_rows(),
                                                 selected_programs, name)
            if dependents:
                message(screen, "Required package",
                        [f"{name} is required by:", ", ".join(dependents)],
                        target_label(values))
                continue
            selected_programs.remove(name)
            if hidden:
                selected_programs.difference_update(hidden)
                message(screen, "Also removed",
                        [f"{name} was needed by programs the menu does not show;",
                         "they are removed too: " + ", ".join(sorted(hidden))],
                        target_label(values))
        else:
            missing = package_select(user_program_rows(), selected_programs,
                                     name)
            if missing is not None:
                message(screen, "Missing package dependency",
                        [f"{name} requires {missing}."],
                        target_label(values))


def select_package_programs(screen, values: dict[str, object]) -> None:
    categories = package_categories(user_program_rows())
    while True:
        choice = choose(screen, "Packages",
                        [label for label, _group in categories] + ["Back"],
                        target_label(values))
        if choice is None or choice == len(categories):
            return
        label, group = categories[choice]
        edit_program_group(screen, values, (group,), label)


def group_rows(values: dict[str, object], groups: tuple[str, ...]) -> list[list[str]]:
    """The programs of some groups that build for the current platform."""
    building = builds_for(str(values["ZEDBSD_PLATFORM"]))
    return [row for row in user_program_rows()
            if row[4] in groups and row[0] in building]


def all_selected(values: dict[str, object], groups: tuple[str, ...]) -> bool:
    """Whether every program of some groups is selected (the All box)."""
    selected = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
    rows = group_rows(values, groups)
    return bool(rows) and all(row[0] in selected for row in rows)


def toggle_all(values: dict[str, object], groups: tuple[str, ...]) -> None:
    """Selects every program of some groups, or with all of them selected
    goes back to their defaults (what a fresh configuration selects)."""
    selected = values.setdefault("ZEDBSD_USER_PROGRAMS", set())
    rows = group_rows(values, groups)
    if all_selected(values, groups):
        for row in rows:
            if row[3] == "y":
                selected.add(row[0])
            else:
                selected.discard(row[0])
        return
    everything = user_program_rows()
    for row in rows:
        if package_select(everything, selected, row[0]) is not None:
            selected.add(row[0])


def program_section(screen, values: dict[str, object], title: str,
                    groups: tuple[str, ...]) -> None:
    """Base or Desktop: All, and Select with the section's programs one by one."""
    while True:
        box = "[*]" if all_selected(values, groups) else "[ ]"
        choice = choose(screen, title, [f"{box} All", "Select", "Back"],
                        target_label(values))
        if choice is None or choice == 2:
            return
        if choice == 0:
            toggle_all(values, groups)
        else:
            edit_program_group(screen, values, groups, title)


def driver_options(screen, values: dict[str, object], rows: list[list[str]],
                   title: str) -> None:
    """Toggles existing driver build options from the shared catalog; an
    always-built (fixed) driver is shown checked and stays built."""
    selected = 0
    while True:
        labels = [("[*]" if row[1] == "fixed" or values.get(row[0]) == "y"
                   else "[ ]") + " " + row[2] for row in rows]
        choice = choose(screen, title, labels + ["Back"],
                        target_label(values), selected)
        if choice is None or choice == len(rows):
            return
        selected = choice
        if rows[choice][1] == "fixed":
            message(screen, title, [rows[choice][2], "",
                                    "is always built into the kernel."],
                    target_label(values))
            continue
        key = rows[choice][0]
        before = driver_firmware(values, str(values["ZEDBSD_PLATFORM"]))
        values[key] = "n" if values.get(key) == "y" else "y"
        follow_driver_firmware(values, before)


def drivers_menu(screen, values: dict[str, object]) -> None:
    """Drivers: Disk, Input, GPU, Audio, Ethernet, WiFi, Bluetooth, USB and Platform."""
    groups = driver_groups(str(values["ZEDBSD_PLATFORM"]))
    selected = 0
    while True:
        choice = choose(screen, "Drivers", [title for title, _rows in groups] + ["Back"],
                        target_label(values), selected)
        if choice is None or choice == len(groups):
            return
        selected = choice
        title, rows = groups[choice]
        driver_options(screen, values, rows, title)


def menu_cpu_labels() -> list[tuple[str, str]]:
    """The CPUs the menu offers, each once and in order, with its name."""
    result: list[tuple[str, str]] = []
    for cpu, label, _platform, _board in MENU_CPUS:
        if (cpu, label) not in result:
            result.append((cpu, label))
    return result


def select_cpu_board(screen, values: dict[str, object]) -> None:
    """CPU / Board: the CPU, and the board it boots on."""
    while True:
        platform = str(values["ZEDBSD_PLATFORM"])
        current = next((item for item in MENU_CPUS if item[2] == platform), None)
        cpu = current[1] if current else platform_record(platform)[1]
        board = current[3] if current else BOARD_LABELS[platform_record(platform)[2]]
        choice = choose(screen, "CPU / Board",
                        [f"CPU: {cpu}", f"Board: {board}", "Back"],
                        target_label(values))
        if choice is None or choice == 2:
            return
        if choice == 0:
            cpus = menu_cpu_labels()
            index = choose(screen, "CPU", [label for _cpu, label in cpus],
                           target_label(values),
                           next((i for i, item in enumerate(cpus)
                                 if current and item[0] == current[0]), 0))
            if index is not None and not (current and current[0] == cpus[index][0]):
                choose_platform(values, next(
                    item[2] for item in MENU_CPUS if item[0] == cpus[index][0]))
            continue
        boards = [item for item in MENU_CPUS
                  if current is not None and item[0] == current[0]]
        if len(boards) < 2:
            message(screen, "Board", [f"{cpu} boots on {board}.",
                                      "The board follows the CPU."],
                    target_label(values))
            continue
        index = choose(screen, "Board", [item[3] for item in boards],
                       target_label(values), boards.index(current))
        if index is not None:
            choose_platform(values, boards[index][2])


def boot_options(screen, values: dict[str, object]) -> None:
    """Boot Option: the graphical boot, the graphical login and the serial mirror."""
    selected = 0
    while True:
        graphical = str(values.get("ZEDBSD_GRAPHICAL_BOOT", "y")) == "y"
        login = str(values.get("ZEDBSD_GRAPHICAL_LOGIN", "y")) == "y"
        mirror = str(values.get("CONFIG_PCAT_SERIAL_MIRROR", "n")) == "y"
        labels = [("[*]" if graphical else "[ ]") + " Graphical boot (No kernel messages on boot console)",
                  ("[*]" if login else "[ ]") + " Graphical login (Automatically starts Keiland)",
                  ("[*]" if mirror else "[ ]") + " Mirror kernel messages to serial port",
                  "Back"]
        choice = choose(screen, "Boot Option", labels, target_label(values), selected)
        if choice is None or choice == 3:
            return
        selected = choice
        if choice == 0:
            values["ZEDBSD_GRAPHICAL_BOOT"] = "n" if graphical else "y"
            values["ZEDBSD_BOOT_KERNEL_MESSAGES"] = "y" if graphical else "n"
        elif choice == 1:
            values["ZEDBSD_GRAPHICAL_LOGIN"] = "n" if login else "y"
        else:
            values["CONFIG_PCAT_SERIAL_MIRROR"] = "n" if mirror else "y"


def development(screen, values: dict[str, object]) -> None:
    """Development: the development files."""
    selected = 0
    while True:
        files = str(values.get("ZEDBSD_ROOTFS_DEVELOPMENT", "y")) == "y"
        labels = [("[*]" if files else "[ ]") + " Install development files (/usr/include, .so links, .pc, .a)",
                  "Back"]
        choice = choose(screen, "Development", labels, target_label(values), selected)
        if choice is None or choice == 1:
            return
        selected = choice
        values["ZEDBSD_ROOTFS_DEVELOPMENT"] = "n" if files else "y"


# The rows of make list-user-programs, read once: the menu asks for them on
# every redraw, and the tree does not change while the menu is open.
USER_PROGRAM_CACHE: list[list[str]] = []


def user_program_rows() -> list[list[str]]:
    if USER_PROGRAM_CACHE:
        return USER_PROGRAM_CACHE
    result = subprocess.run(
        ["make", "--no-print-directory", "list-user-programs"], cwd=REPO,
        check=False, text=True, stdout=subprocess.PIPE,
    )
    if result.returncode != 0:
        raise SystemExit("cannot obtain the userland package list from Make")
    rows = []
    for number, line in enumerate(result.stdout.splitlines(), 1):
        parts = line.split("|")
        if len(parts) != 7:
            raise SystemExit(f"make list-user-programs:{number}: malformed row")
        rows.append(parts)
    USER_PROGRAM_CACHE.extend(rows)
    return rows


# Firmware licenses (plan/ws233/phase002/phase.md).  The build fetches a
# firmware package only after its license is accepted once; make records the
# acceptance and asks on its standard input, which this menu cannot give it,
# so the menu asks first, in a screen of its own, through the same script.
LICENSE_SMALLEST = (12, 40)
LICENSE_STATEMENT = ("By selecting Accept, you confirm that you have read and agree "
                     "to be bound by the license terms above.")
LICENSE_STATEMENT_SHORT = "I have read and accept the license terms above."


def license_script() -> Path:
    return REPO / "userland" / "firmware" / "license-agree.sh"


def firmware_licenses_pending(config: Path, extra: tuple[str, ...] = ()) -> list[dict[str, str]] | str:
    """The selected firmware whose license the build would ask about, each with
    its notice, its checked license text and its record (make
    firmware-licenses-pending, which asks nothing), or the reason it failed."""
    result = subprocess.run(
        ["make", "--no-print-directory", "-s", f"ZEDBSD_CONFIG={config}", *extra,
         "firmware-licenses-pending"],
        cwd=REPO, check=False, text=True, errors="replace",
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if result.returncode != 0:
        return (result.stderr.strip() or f"make exited with {result.returncode}")
    entries = []
    keys = ("package", "notice", "text", "record", "name", "size", "sha256")
    for line in result.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != len(keys):
            return f"malformed line from make firmware-licenses-pending: {line}"
        entry = dict(zip(keys, fields))
        for key in ("notice", "text", "record"):
            entry[key] = str(REPO / entry[key])
        entries.append(entry)
    return entries


def accept_license(entry: dict[str, str]) -> str | None:
    """Records that the license shown was accepted (license-agree.sh, mode
    accept, which alone writes the record); None, or the reason it failed."""
    result = subprocess.run(
        ["sh", str(license_script()), entry["package"], entry["record"], entry["name"],
         entry["size"], entry["sha256"]],
        cwd=REPO, check=False, text=True, errors="replace",
        env=dict(os.environ, ZEDBSD_FIRMWARE_LICENSE_MODE="accept"),
        stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        return result.stdout.strip() or f"license-agree.sh exited with {result.returncode}"
    return None


def license_document(entry: dict[str, str]) -> list[tuple[str, str]]:
    """The agreement's text as (line, kind) pairs, kind heading or body: what
    the firmware is, its copyright notice, and the whole license as written."""
    notice = Path(entry["notice"]).read_text(encoding="utf-8", errors="replace").rstrip("\n")
    description, _blank, copyright_notice = notice.partition("\n\n")
    license_text = Path(entry["text"]).read_text(encoding="utf-8", errors="replace").rstrip("\n")
    document: list[tuple[str, str]] = [("1. THE FIRMWARE", "heading"), ("", "body")]
    document += [(line, "field") for line in description.splitlines()]
    document += [("", "body"), ("2. COPYRIGHT NOTICE", "heading"), ("", "body")]
    document += [(line, "body") for line in copyright_notice.splitlines()]
    document += [("", "body"), (f"3. LICENSE ({entry['name']})", "heading"), ("", "body")]
    document += [(line, "body") for line in license_paragraphs(license_text)]
    return document


# A list item or heading of a license: a bullet, a run of #, or (1), (a), (iv), 1. or a).
LICENSE_ITEM = re.compile(r"^(\s*)(?:[*\-•]|#+|\((?:[0-9]{1,2}|[a-z]|[ivx]{1,4})\)|(?:[0-9]{1,2}|[a-z])[.)])\s+")


def license_paragraphs(text: str) -> list[str]:
    """The license with the line breaks of its filled paragraphs taken out,
    so that a narrow screen folds whole paragraphs instead of leaving a word
    on a line of its own; every word, blank line, list item and short line
    (a line that ends well before the text's width, as an address or a
    copyright line does) stays as written."""
    lines = [line.expandtabs(8).rstrip() for line in text.splitlines()]
    longest = max((len(line) for line in lines), default=0)
    paragraphs: list[str] = []
    continuation = -1
    previous = 0
    for line in lines:
        indent = len(line) - len(line.lstrip())
        item = LICENSE_ITEM.match(line)
        joins = (line and paragraphs and paragraphs[-1] and not item and
                 indent == continuation and previous >= longest * 6 // 10)
        previous = len(line)
        if joins:
            paragraphs[-1] += " " + line.strip()
            continue
        paragraphs.append(line)
        continuation = len(item.group(0)) if item else indent
        if not line:
            continuation = -1
    return paragraphs


def wrap_document(document: list[tuple[str, str]], width: int) -> list[tuple[str, str]]:
    """The document folded to the width, every line kept as written and only
    broken where it is longer; a continuation keeps the line's indent."""
    rows = []
    for line, kind in document:
        line = line.expandtabs(8).rstrip()
        if len(line) <= width:
            rows.append((line, kind))
            continue
        indent = line[:len(line) - len(line.lstrip())]
        field = re.match(r"^\w[\w ]*:\s+", line) if kind == "field" else LICENSE_ITEM.match(line)
        if field:
            indent = " " * len(field.group(0))
        if len(indent) > width // 2:
            indent = ""
        for part in textwrap.wrap(line, width, subsequent_indent=indent,
                                  break_long_words=True, break_on_hyphens=False) or [""]:
            rows.append((part, kind))
    return rows


def license_visible(height: int, width: int) -> int:
    """How many lines of the agreement the screen shows (0: too small): the
    frame takes the rows between the two heading rows and the five rows of
    position, statement (two), buttons and hint."""
    if height < LICENSE_SMALLEST[0] or width < LICENSE_SMALLEST[1]:
        return 0
    return height - 9


def license_statement(width: int) -> list[str]:
    """The statement above the buttons, in at most two lines of the width."""
    lines = textwrap.wrap(LICENSE_STATEMENT, width)
    return lines if len(lines) <= 2 else textwrap.wrap(LICENSE_STATEMENT_SHORT, width)[:2]


def draw_license(screen, entry: dict[str, str], number: int, count: int,
                 rows: list[tuple[str, str]], offset: int, enabled: bool,
                 focus: int) -> None:
    """Draws the agreement, its text from line offset of rows."""
    screen.erase()
    height, width = screen.getmaxyx()
    if license_visible(height, width) == 0:
        lines = textwrap.wrap(
            "The terminal is too small to show the license (at least "
            f"{LICENSE_SMALLEST[1]}x{LICENSE_SMALLEST[0]}). Enlarge it, or press D to decline.",
            max(1, width))
        for row, line in enumerate(lines[:height]):
            addstr(screen, row, 0, line, width)
        screen.refresh()
        return
    title = " Firmware License Agreement "
    addstr(screen, 0, 0, " " * width, width, curses.A_REVERSE)
    addstr(screen, 0, max(0, (width - len(title)) // 2), title, width,
           curses.A_REVERSE | curses.A_BOLD)
    counter = f"License {number} of {count}"
    addstr(screen, 1, 2, entry["package"], width - 4 - len(counter) - 2, curses.A_BOLD)
    addstr(screen, 1, max(2, width - 2 - len(counter)), counter, len(counter))
    top, bottom = 2, height - 6
    visible = license_visible(height, width)
    inner = width - 6
    addstr(screen, top, 1, "┌" + "─" * (width - 4) + "┐", width - 2)
    for row in range(visible):
        y = top + 1 + row
        addstr(screen, y, 1, "│", 1)
        addstr(screen, y, width - 2, "│", 1)
        index = offset + row
        if index < len(rows):
            text, kind = rows[index]
            attr = curses.A_BOLD if kind == "heading" else curses.A_NORMAL
            addstr(screen, y, 3, text, inner, attr)
    addstr(screen, bottom, 1, "└" + "─" * (width - 4) + "┘", width - 2)
    last = min(len(rows), offset + visible)
    percent = 100 * last // max(1, len(rows))
    position = f"Lines {offset + 1}-{last} of {len(rows)} ({percent}%)"
    keys = "↑↓ PgUp PgDn Home End"
    addstr(screen, bottom + 1, 2, position, width - 4)
    if len(position) + len(keys) + 6 <= width:
        addstr(screen, bottom + 1, width - 2 - len(keys), keys, len(keys))
    for row, line in enumerate(license_statement(width - 4)):
        addstr(screen, bottom + 2 + row, 2, line, width - 4)
    accept, decline = "[ Accept ]", "[ Decline ]"
    start = max(2, (width - len(accept) - len(decline) - 4) // 2)
    accept_attr = curses.A_DIM if not enabled else curses.A_NORMAL
    if focus == 0:
        accept_attr |= curses.A_REVERSE
    addstr(screen, bottom + 4, start, accept, width - start, accept_attr)
    addstr(screen, bottom + 4, start + len(accept) + 4, decline,
           width - start - len(accept) - 4,
           curses.A_REVERSE if focus == 1 else curses.A_NORMAL)
    hint = ("Tab switches, Enter confirms, D declines." if enabled else
            "Scroll to the end of the license to enable Accept.")
    if len(hint) > width - 4:
        hint = "Tab, Enter, D: decline." if enabled else "Scroll to the end to accept."
    addstr(screen, height - 1, 2, hint, width - 4)
    screen.refresh()


def license_screen(screen, entry: dict[str, str], number: int = 1, count: int = 1) -> bool:
    """Shows one firmware's license agreement and returns whether it was
    accepted.  Accept can be chosen only once the end of the license has been
    on the screen, so the whole text was put before the reader; Decline has
    the focus first, so a stray Enter does not agree."""
    document = license_document(entry)
    offset = 0
    focus = 1
    enabled = False
    while True:
        height, width = screen.getmaxyx()
        rows = wrap_document(document, max(1, width - 6))
        visible = license_visible(height, width)
        if visible > 0:
            offset = max(0, min(offset, len(rows) - visible))
            if offset + visible >= len(rows):
                enabled = True
        draw_license(screen, entry, number, count, rows, offset, enabled, focus)
        key = screen.getch()
        page = max(1, visible - 1)
        if key in (ord("d"), ord("D"), ord("q"), ord("Q"), 27):
            return False
        if visible == 0:
            continue
        if key in (curses.KEY_UP, ord("k")):
            offset -= 1
        elif key in (curses.KEY_DOWN, ord("j")):
            offset += 1
        elif key in (curses.KEY_PPAGE, ord("b")):
            offset -= page
        elif key in (curses.KEY_NPAGE, ord(" ")):
            offset += page
        elif key in (curses.KEY_HOME, ord("g")):
            offset = 0
        elif key in (curses.KEY_END, ord("G")):
            offset = len(rows)
        elif key in (curses.KEY_LEFT, curses.KEY_RIGHT, 9, curses.KEY_BTAB):
            focus = 1 - focus
        elif key in (ord("a"), ord("A")) and enabled:
            return True
        elif key in (10, 13, curses.KEY_ENTER):
            if focus == 1:
                return False
            if enabled:
                return True
        offset = max(0, offset)


def declined_lines(entry: dict[str, str], unread: list[dict[str, str]]) -> list[str]:
    """What the build menu says when a license is declined."""
    package = entry["package"]
    lines = [f"The license of {package} was declined,", "so the boot image was not built.", ""]
    if package == "rpi4-boot-firmware":
        lines += ["A Raspberry Pi 4 cannot boot without its boot firmware,",
                  "so it cannot be left out of the image."]
    else:
        lines += [f"To build without it, turn {package} off in the",
                  "Firmware menu and choose Build boot image again."]
    if unread:
        lines += ["", "Not yet shown: " + ", ".join(item["package"] for item in unread)]
    return lines


def settle_firmware_licenses(screen, values: dict[str, object], output: Path) -> bool:
    """Shows the license of each selected firmware not yet accepted and
    records each acceptance; False (after saying why) when the build must not
    start."""
    pending = firmware_licenses_pending(output)
    if isinstance(pending, str):
        message(screen, "Firmware licenses",
                ["The firmware licenses could not be read:", ""] + pending.splitlines()[-12:],
                target_label(values))
        return False
    for index, entry in enumerate(pending):
        if not license_screen(screen, entry, index + 1, len(pending)):
            message(screen, "Build canceled", declined_lines(entry, pending[index + 1:]),
                    target_label(values))
            return False
        error = accept_license(entry)
        if error is not None:
            message(screen, "Firmware licenses",
                    [f"The acceptance of {entry['package']} could not be recorded:", ""] +
                    error.splitlines()[-12:], target_label(values))
            return False
    return True


def build_log(values: dict[str, object]) -> Path:
    """Where Build boot image writes make's output: the build directory of the target."""
    platform = str(values["ZEDBSD_PLATFORM"])
    directory = {"i386": "pcat", "rpi4": "arm64", "sun4u": "sparcv9"}.get(platform, platform)
    return REPO / "build" / directory / "menuconfig-build.log"


def trace_target(line: str) -> str | None:
    """The target a line of make --trace says is being made, or None."""
    match = re.search(r"update target '([^']+)'", line)
    return match.group(1) if match else None


def draw_progress(screen, values: dict[str, object], done: int, total: int,
                  current: str) -> None:
    """The build's progress: a bar of the targets made of those to make, and the one being made now."""
    screen.erase()
    height, width = screen.getmaxyx()
    draw_title(screen, "Build boot image", target_label(values))
    total = max(total, done, 1)
    span = max(10, width - 20)
    filled = span * done // total
    addstr(screen, 4, 4, "[" + "#" * filled + "-" * (span - filled) + "]", width - 8)
    addstr(screen, 5, 4, f"{done} of {total} targets ({100 * done // total}%)", width - 8)
    addstr(screen, 7, 4, "Now building:", width - 8, curses.A_BOLD)
    addstr(screen, 8, 6, current, width - 10)
    addstr(screen, 10, 4, f"Log: {build_log(values)}", width - 8)
    screen.refresh()


def build_boot_image(screen, values: dict[str, object], output: Path) -> None:
    """Saves the configuration and builds the boot image with as many jobs as
    the machine has processors (2026-10-09 user: this menu only), showing a
    progress bar and the target being built."""
    answer = choose(screen, "Build boot image: are you sure?",
                    ["Yes", "No"], target_label(values))
    if answer != 0:
        return
    save(output, values)
    if not settle_firmware_licenses(screen, values, output):
        return
    jobs = os.environ.get("ZEDBSD_JOBS") or str(os.cpu_count() or 1)
    common = ["make", "--no-print-directory", "--trace", f"ZEDBSD_CONFIG={output}"]
    draw_progress(screen, values, 0, 1, "counting what to build...")
    # make never reads this menu's terminal: a question it would ask (a firmware
    # license not settled above) fails at once and says why in the log.
    planned = subprocess.run(common + ["-n", "disk-image"], cwd=REPO, check=False,
                             text=True, stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    total = sum(1 for line in planned.stdout.splitlines() if trace_target(line))
    log = build_log(values)
    log.parent.mkdir(parents=True, exist_ok=True)
    done = 0
    current = "starting"
    status = None
    try:
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(common + [f"-j{jobs}", "disk-image"], cwd=REPO,
                                       stdin=subprocess.DEVNULL,
                                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                       text=True, errors="replace")
            assert process.stdout is not None
            for line in process.stdout:
                stream.write(line)
                target = trace_target(line)
                if target:
                    done += 1
                    current = target
                    draw_progress(screen, values, done, total, current)
            status = process.wait()
    except OSError as error:
        message(screen, "Build result", [f"Build failed to start: {error}"], target_label(values))
        return
    if status == 0:
        message(screen, "Build result",
                ["Build succeeded.", "", f"{done} targets built.", f"Log: {log}"],
                target_label(values))
        return
    tail = log.read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
    message(screen, "Build result",
            [f"Build failed (status {status}).", f"Log: {log}", ""] + tail,
            target_label(values))


def tui(screen, values: dict[str, object], output: Path) -> None:
    curses.curs_set(0)
    screen.keypad(True)
    every_entry = [
        ("CPU / Board", "cpu-board"),
        ("Boot Option", "boot"),
        ("Drivers", "drivers"),
        ("Development", "development"),
        ("Base", "base"),
        ("Retro", "retro"),
        ("Desktop", "desktop"),
        ("Packages", "packages"),
        ("Firmware", "firmware"),
        ("", ""),
        ("Build boot image", "build"),
        ("", ""),
        ("Exit", "exit"),
    ]
    selected = 0
    while True:
        # The Desktop entry is shown only where the Desktop is built.
        entries = [entry for entry in every_entry
                   if entry[1] != "desktop" or
                   desktop_builds(str(values["ZEDBSD_PLATFORM"]))]
        labels = [label for label, _action in entries]
        choice = choose(screen, "Main menu", labels, target_label(values), selected)
        if choice is None:
            save(output, values)
            return
        selected = choice
        action = entries[choice][1]
        if action == "exit":
            save(output, values)
            return
        if action == "retro":
            edit_programs(screen, values, retro_rows(values), "Retro")
        elif action == "cpu-board":
            select_cpu_board(screen, values)
        elif action == "boot":
            boot_options(screen, values)
        elif action == "drivers":
            drivers_menu(screen, values)
        elif action == "development":
            development(screen, values)
        elif action == "base":
            program_section(screen, values, "Base", BASE_GROUPS)
        elif action == "desktop":
            program_section(screen, values, "Desktop", DESKTOP_GROUPS)
        elif action == "packages":
            select_package_programs(screen, values)
        elif action == "firmware":
            edit_program_group(screen, values, FIRMWARE_GROUPS, "Firmware")
        elif action == "build":
            build_boot_image(screen, values, output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--defaults", action="store_true",
                        help="write defaults without opening the TUI")
    args = parser.parse_args()
    values = defaults() if args.defaults else load(args.output)
    if args.defaults:
        save(args.output, values)
    else:
        curses.wrapper(tui, values, args.output)


if __name__ == "__main__":
    main()
