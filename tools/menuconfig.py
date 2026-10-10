#!/usr/bin/env python3
"""Curses build menu and configuration editor for zedBSD."""
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib

from __future__ import annotations

import argparse
import curses
import os
import re
import subprocess
from pathlib import Path


REPO = Path(__file__).resolve().parent.parent
CONFIG_DIR = REPO / "config"

PLATFORMS = [
    ("i386", "i386", "pcat", "i386 PC/AT compatible"),
    ("amd64", "amd64", "pcat", "x86_64 PC/AT compatible"),
    ("pc98", "i386", "pc98", "NEC PC-9800"),
    ("rpi4", "arm64", "rpi4", "Raspberry Pi 4 Arm64"),
    ("sun4u", "sparcv9", "sun4u", "SPARC V9 sun4u"),
    ("x68k", "m68k", "x68k", "Sharp X68000 MC68030"),
]

BOARD_LABELS = {
    "pcat": "IBM PC/AT compatible",
    "pc98": "NEC PC-9800",
    "rpi4": "Raspberry Pi 4",
    "sun4u": "sun4u",
    "x68k": "Sharp X68000",
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
    ("sparcv9", "sun4u"): [("default", "Default")],
    ("m68k", "x68k"): [("default", "Default")],
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

# The CPUs and the board each one boots on (2026-10-09 user, WS193): the
# platform each pair builds.  Another platform read from a config.mk is kept
# as it is and shown as the current one.
MENU_CPUS = [
    ("amd64", "x86_64", "amd64", "UEFI + ACPI"),
    ("arm64", "arm64", "rpi4", "Raspberry Pi 4"),
]

# The user programs' groups the Base, Desktop and Firmware menus select
# from.  The X11 and test programs, the disk layout (Variant), the kernel's
# options, the drivers, the test hooks and Noct's GPU accelerator are not in
# the menu (2026-10-09 user: "menu から外す", "X11とTestsはメニューから削除し、
# 直接記述のみにする"): they are set in config.mk by hand, and the values a
# config.mk holds are read and written back as they are.
BASE_GROUPS = ("base", "comp")
DESKTOP_GROUPS = ("desktop",)
FIRMWARE_GROUPS = ("firmware",)


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


def architecture_driver_path(platform: str) -> Path:
    architecture = platform_record(platform)[1]
    directory = CONFIG_DIR / "drivers" / "architecture"
    platform_path = directory / f"{platform}.drivers"
    return platform_path if platform_path.exists() else directory / f"{architecture}.drivers"


def all_option_files() -> list[Path]:
    result = [CONFIG_DIR / "kernel-options.list",
              CONFIG_DIR / "rootfs-options.list",
              CONFIG_DIR / "drivers" / "isa.drivers",
              CONFIG_DIR / "drivers" / "pci.drivers",
              CONFIG_DIR / "drivers" / "usb.drivers",
              CONFIG_DIR / "drivers" / "generic.drivers"]
    result.extend(sorted((CONFIG_DIR / "drivers" / "architecture").glob("*.drivers")))
    return result


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
    programs = user_program_rows()
    values["ZEDBSD_USER_PROGRAMS"] = {
        row[0] for row in programs if row[3] == "y"
    }
    return values


def load(path: Path) -> dict[str, object]:
    values = defaults()
    if not path.exists():
        return values
    pattern = re.compile(r"^([A-Z][A-Z0-9_]*)\s*(?::=|=)\s*(.*?)\s*$")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if not match:
            continue
        key, value = match.groups()
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
    supported = set()
    platform_option_files = [CONFIG_DIR / "kernel-options.list",
                             architecture_driver_path(platform)]
    common_option_files = [CONFIG_DIR / "drivers" / "isa.drivers",
                           CONFIG_DIR / "drivers" / "pci.drivers",
                           CONFIG_DIR / "drivers" / "usb.drivers",
                           CONFIG_DIR / "drivers" / "generic.drivers"]
    for path in platform_option_files:
        for key, kind, _label, targets, _default, _choices in read_rows(path, 6):
            if key != "-" and kind != "fixed" and applies(targets, platform):
                supported.add(key)
    for path in common_option_files:
        for key, kind, _label, targets, _default, _choices in read_rows(path, 6):
            if (key != "-" and kind != "fixed" and
                    applies(targets, platform)):
                supported.add(key)
    for key in list(values):
        if key.startswith("CONFIG_DRIVER_") and key not in supported:
            values[key] = "n"
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
    ordered = [row[0] for row in user_program_rows()
               if row[0] in programs and applies(row[2], platform)]
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


def edit_program_group(screen, values: dict[str, object], groups: tuple[str, ...],
                       title: str) -> None:
    platform = str(values["ZEDBSD_PLATFORM"])
    rows = [row for row in user_program_rows()
            if row[4] in groups and applies(row[2], platform)]
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
            dependents = package_dependents(user_program_rows(),
                                            selected_programs, name)
            if dependents:
                message(screen, "Required package",
                        [f"{name} is required by:", ", ".join(dependents)],
                        target_label(values))
                continue
            selected_programs.remove(name)
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
    platform = str(values["ZEDBSD_PLATFORM"])
    return [row for row in user_program_rows()
            if row[4] in groups and applies(row[2], platform)]


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
            index = choose(screen, "CPU", [item[1] for item in MENU_CPUS],
                           target_label(values),
                           next((i for i, item in enumerate(MENU_CPUS)
                                 if item[2] == platform), 0))
            if index is not None:
                values["ZEDBSD_PLATFORM"] = MENU_CPUS[index][2]
                normalize(values)
        else:
            message(screen, "Board", [f"{cpu} boots on {board}.",
                                      "The board follows the CPU."],
                    target_label(values))


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
    jobs = os.environ.get("ZEDBSD_JOBS") or str(os.cpu_count() or 1)
    common = ["make", "--no-print-directory", "--trace", f"ZEDBSD_CONFIG={output}"]
    draw_progress(screen, values, 0, 1, "counting what to build...")
    planned = subprocess.run(common + ["-n", "disk-image"], cwd=REPO, check=False,
                             text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    total = sum(1 for line in planned.stdout.splitlines() if trace_target(line))
    log = build_log(values)
    log.parent.mkdir(parents=True, exist_ok=True)
    done = 0
    current = "starting"
    status = None
    try:
        with log.open("w", encoding="utf-8") as stream:
            process = subprocess.Popen(common + [f"-j{jobs}", "disk-image"], cwd=REPO,
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
    entries = [
        ("CPU / Board", "cpu-board"),
        ("Boot Option", "boot"),
        ("Development", "development"),
        ("Base", "base"),
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
        if action == "cpu-board":
            select_cpu_board(screen, values)
        elif action == "boot":
            boot_options(screen, values)
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
