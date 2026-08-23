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

ARCHITECTURES = [
    ("i386", "i386 32-bit"),
    ("amd64", "x86_64 64-bit"),
    ("arm64", "Arm64"),
    ("sparcv9", "SPARC V9 64-bit"),
    ("m68k", "Motorola 68000 family"),
]

BOARD_LABELS = {
    "pcat": "IBM PC/AT compatible",
    "pc98": "NEC PC-9800",
    "rpi4": "Raspberry Pi 4",
    "sun4u": "sun4u",
    "x68k": "Sharp X68000",
}

DRIVER_CATEGORIES = [
    ("Architecture drivers", None),
    ("ISA drivers", CONFIG_DIR / "drivers" / "isa.drivers"),
    ("PCI drivers", CONFIG_DIR / "drivers" / "pci.drivers"),
    ("USB drivers", CONFIG_DIR / "drivers" / "usb.drivers"),
    ("Generic drivers", CONFIG_DIR / "drivers" / "generic.drivers"),
]

PROGRAM_CATEGORIES = [
    ("Base", "base"),
    ("Compilers", "comp"),
    ("X11 servers", "x11-servers"),
    ("X11 applications", "x11-applications"),
    ("Packages", "packages"),
]

PACKAGE_CATEGORIES = [
    ("Languages", "packages/lang"),
    ("Editors", "packages/editors"),
]


def platform_record(name: str):
    return next((item for item in PLATFORMS if item[0] == name), PLATFORMS[0])


def platforms_for_architecture(architecture: str):
    return [item for item in PLATFORMS if item[1] == architecture]


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


def option_rows(path: Path, platform: str,
                platform_specific: bool = True) -> list[dict[str, object]]:
    result = []
    for key, kind, label, targets, default, choices in read_rows(path, 6):
        if platform_specific and not applies(targets, platform):
            continue
        parsed_choices = []
        if choices:
            for item in choices.split(","):
                value, text = item.split(":", 1)
                parsed_choices.append((value, text))
        result.append({"key": key, "kind": kind, "label": label,
                       "default": default, "choices": parsed_choices})
    return result


def architecture_driver_path(platform: str) -> Path:
    architecture = platform_record(platform)[1]
    directory = CONFIG_DIR / "drivers" / "architecture"
    platform_path = directory / f"{platform}.drivers"
    return platform_path if platform_path.exists() else directory / f"{architecture}.drivers"


def all_option_files() -> list[Path]:
    result = [CONFIG_DIR / "kernel-options.list",
              CONFIG_DIR / "drivers" / "isa.drivers",
              CONFIG_DIR / "drivers" / "pci.drivers",
              CONFIG_DIR / "drivers" / "usb.drivers",
              CONFIG_DIR / "drivers" / "generic.drivers"]
    result.extend(sorted((CONFIG_DIR / "drivers" / "architecture").glob("*.drivers")))
    return result


def defaults() -> dict[str, object]:
    values: dict[str, object] = {"ZEDBSD_PLATFORM": "i386"}
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
        values["ZEDBSD_PLATFORM"] = "i386"
    return values


def normalize(values: dict[str, object]) -> None:
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
        for key, kind, _label, _targets, _default, _choices in read_rows(path, 6):
            if key != "-" and kind != "fixed":
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
        "ZEDBSD_MENU_VERSION := 2",
        f"ZEDBSD_PLATFORM := {platform}",
        f"ZEDBSD_ARCHITECTURE := {architecture}",
        f"ZEDBSD_BOARD := {board}",
        "",
    ]
    emitted = set()
    for option_file in all_option_files():
        for key, kind, _label, _targets, _default, _choices in read_rows(option_file, 6):
            if key == "-" or kind == "fixed" or key in emitted:
                continue
            lines.append(f"{key} := {values.get(key, 'n')}")
            emitted.add(key)
    programs = values.get("ZEDBSD_USER_PROGRAMS", set())
    ordered = [row[0] for row in user_program_rows() if row[0] in programs]
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


def select_target(screen, values: dict[str, object]) -> None:
    while True:
        platform = str(values["ZEDBSD_PLATFORM"])
        record = platform_record(platform)
        labels = [f"Architecture: {record[1]}",
                  f"Board: {BOARD_LABELS[record[2]]}", "Back"]
        selected = choose(screen, "Select target", labels, target_label(values))
        if selected is None or selected == 2:
            return
        if selected == 0:
            index = choose(screen, "Architecture",
                           [label for _name, label in ARCHITECTURES],
                           target_label(values),
                           next((i for i, item in enumerate(ARCHITECTURES)
                                 if item[0] == record[1]), 0))
            if index is not None:
                values["ZEDBSD_PLATFORM"] = platforms_for_architecture(
                    ARCHITECTURES[index][0])[0][0]
                normalize(values)
        else:
            candidates = platforms_for_architecture(record[1])
            index = choose(screen, "Board",
                           [BOARD_LABELS[item[2]] for item in candidates],
                           target_label(values),
                           next((i for i, item in enumerate(candidates)
                                 if item[0] == platform), 0))
            if index is not None:
                values["ZEDBSD_PLATFORM"] = candidates[index][0]
                normalize(values)


def option_value(option: dict[str, object], values: dict[str, object]) -> str:
    kind, key = str(option["kind"]), str(option["key"])
    if kind == "fixed":
        return "[*]"
    value = str(values.get(key, option["default"]))
    if kind == "bool":
        return "[*]" if value == "y" else "[ ]"
    for candidate, label in option["choices"]:
        if candidate == value:
            return f"<{label}>"
    return f"<{value}>"


def edit_options(screen, title: str, options: list[dict[str, object]],
                 values: dict[str, object]) -> None:
    selected = 0
    while True:
        labels = [f"{option_value(option, values):<18} {option['label']}"
                  for option in options]
        labels.append("Back")
        selected_result = choose(screen, title, labels, target_label(values), selected)
        if selected_result is None or selected_result == len(options):
            return
        selected = selected_result
        option = options[selected]
        key, kind = str(option["key"]), str(option["kind"])
        if kind == "fixed" or key == "-":
            continue
        if kind == "bool":
            values[key] = "n" if str(values.get(key, "n")) == "y" else "y"
        elif kind == "choice":
            choices = option["choices"]
            current = str(values.get(key, option["default"]))
            index = next((i for i, item in enumerate(choices)
                          if item[0] == current), 0)
            chosen = choose(screen, str(option["label"]),
                            [item[1] for item in choices], target_label(values), index)
            if chosen is not None:
                values[key] = choices[chosen][0]


def select_drivers(screen, values: dict[str, object]) -> None:
    while True:
        selected = choose(screen, "Select drivers",
                          [name for name, _path in DRIVER_CATEGORIES] + ["Back"],
                          target_label(values))
        if selected is None or selected == len(DRIVER_CATEGORIES):
            return
        title, path = DRIVER_CATEGORIES[selected]
        if path is None:
            path = architecture_driver_path(str(values["ZEDBSD_PLATFORM"]))
        options = option_rows(path, str(values["ZEDBSD_PLATFORM"]),
                              platform_specific=path == architecture_driver_path(
                                  str(values["ZEDBSD_PLATFORM"])))
        edit_options(screen, title, options, values)


def edit_program_group(screen, values: dict[str, object], group: str,
                       title: str) -> None:
    platform = str(values["ZEDBSD_PLATFORM"])
    rows = [row for row in user_program_rows() if row[4] == group]
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
        name, _label, _targets, _default, _group, package, requirements = rows[choice]
        if name in selected_programs:
            dependents = [row[1] for row in user_program_rows()
                          if row[0] in selected_programs and package and
                          package in row[6].split()]
            if dependents:
                message(screen, "Required package",
                        [f"{name} is required by:", ", ".join(dependents)],
                        target_label(values))
                continue
            selected_programs.remove(name)
        else:
            selected_programs.add(name)
            pending = requirements.split()
            all_rows = user_program_rows()
            while pending:
                requirement = pending.pop(0)
                dependency = next((row for row in all_rows
                                   if row[5] == requirement), None)
                if dependency is None:
                    message(screen, "Missing package dependency",
                            [f"{name} requires {requirement}."],
                            target_label(values))
                    selected_programs.remove(name)
                    break
                if dependency[0] not in selected_programs:
                    selected_programs.add(dependency[0])
                    pending.extend(dependency[6].split())


def select_package_programs(screen, values: dict[str, object]) -> None:
    while True:
        choice = choose(screen, "Packages",
                        [label for label, _group in PACKAGE_CATEGORIES] + ["Back"],
                        target_label(values))
        if choice is None or choice == len(PACKAGE_CATEGORIES):
            return
        label, group = PACKAGE_CATEGORIES[choice]
        edit_program_group(screen, values, group, label)


def select_programs(screen, values: dict[str, object]) -> None:
    while True:
        choice = choose(screen, "Select user programs",
                        [label for label, _group in PROGRAM_CATEGORIES] + ["Back"],
                        target_label(values))
        if choice is None or choice == len(PROGRAM_CATEGORIES):
            return
        label, group = PROGRAM_CATEGORIES[choice]
        if group == "packages":
            select_package_programs(screen, values)
        else:
            edit_program_group(screen, values, group, label)


def user_program_rows() -> list[list[str]]:
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
    return rows


def build(screen, values: dict[str, object], output: Path,
          make_target: str, artifact_target: str) -> None:
    answer = choose(screen, "Are you sure you want to build?",
                    ["Yes", "No"], target_label(values))
    if answer != 0:
        return
    save(output, values)
    curses.def_prog_mode()
    curses.endwin()
    print(f"\nBuilding {target_label(values)}...\n", flush=True)
    try:
        jobs = os.environ.get("ZEDBSD_JOBS") or str(os.cpu_count() or 1)
        result = subprocess.run(
            ["make", f"-j{jobs}", "--no-print-directory",
             f"ZEDBSD_CONFIG={output}", make_target], cwd=REPO,
            check=False)
    except OSError as error:
        print(f"build menu: {error}", flush=True)
        result = None
    finally:
        curses.reset_prog_mode()
        curses.curs_set(0)
        screen.clear()
        screen.refresh()
    if result is not None and result.returncode == 0:
        artifacts = subprocess.run(
            ["make", "--no-print-directory", f"ZEDBSD_CONFIG={output}",
             artifact_target], cwd=REPO, check=False, text=True,
            stdout=subprocess.PIPE).stdout.splitlines()
        lines = ["Build succeeded.", ""] + [
            artifact for artifact in artifacts if artifact
        ]
        message(screen, "Build result", lines, target_label(values))
    else:
        status = result.returncode if result is not None else "not started"
        message(screen, "Build result", ["Build failed.", f"Status: {status}"],
                target_label(values))


def tui(screen, values: dict[str, object], output: Path) -> None:
    curses.curs_set(0)
    screen.keypad(True)
    while True:
        entries = [
            ("Build all", "build-all"),
            ("", ""),
            ("Select target", "target"),
            ("Select kernel option", "kernel-options"),
            ("Select drivers", "drivers"),
            ("Select user programs", "user-programs"),
            ("Build toolchain", "build-toolchain"),
            ("Build kernel", "build-kernel"),
            ("Build rootfs", "build-rootfs"),
            ("Build rootfs image", "build-rootfs-image"),
            ("Build boot disk image", "build-boot-disk"),
            ("", ""),
            ("Save and exit", "save"),
        ]
        labels = [label for label, _action in entries]
        selected = choose(screen, "Main menu", labels, target_label(values))
        if selected is None:
            save(output, values)
            return
        action = entries[selected][1]
        if action == "save":
            save(output, values)
            return
        if action == "build-all":
            build(screen, values, output, "build-release",
                  "print-release-artifacts")
        elif action == "target":
            select_target(screen, values)
        elif action == "kernel-options":
            edit_options(screen, "Select kernel option",
                         option_rows(CONFIG_DIR / "kernel-options.list",
                                     str(values["ZEDBSD_PLATFORM"])), values)
        elif action == "drivers":
            select_drivers(screen, values)
        elif action == "user-programs":
            select_programs(screen, values)
        elif action == "build-toolchain":
            message(screen, "Build toolchain",
                    ["Build toolchain is not implemented yet."],
                    target_label(values))
        elif action == "build-kernel":
            build(screen, values, output, "build-kernel",
                  "print-kernel-artifact")
        elif action == "build-rootfs":
            build(screen, values, output, "build-rootfs",
                  "print-rootfs-artifact")
        elif action == "build-rootfs-image":
            build(screen, values, output, "build-rootfs-image",
                  "print-rootfs-image-artifact")
        elif action == "build-boot-disk":
            build(screen, values, output, "build-boot-disk-image",
                  "print-boot-disk-artifact")


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
