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
    expected = ["CPU / Board", "Boot Option", "Drivers", "Development", "Base", "Retro", "Desktop",
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
        fail("a selection (Retro's zterm, the kernel options) was not kept")
    values = menu.defaults()
    values["ZEDBSD_USER_PROGRAMS"].add("zgears")
    menu.save(path, values)
    if "zgears" not in menu.load(path)["ZEDBSD_USER_PROGRAMS"]:
        fail("an X11 program the menu does not show (zgears) was not kept")

    if menu.trace_target("Makefile:12: update target 'build/amd64/vmunix' due to: x") != "build/amd64/vmunix":
        fail("the build's progress does not read make's trace")
    if menu.trace_target("cc -c x.c") is not None:
        fail("the build's progress counts a line that is no target")


def walk(function, answers: list, values: dict[str, object]) -> list[tuple[str, list[str]]]:
    """Runs one submenu with choose() answering from a list, each answer a
    label to pick (or None to go back), and returns what the menu showed."""
    shown: list[tuple[str, list[str]]] = []
    pending = list(answers)

    def choose(_screen, title, labels, _target, _selected=0):
        shown.append((title, list(labels)))
        if not pending:
            fail(f"the {title} walk did not end: {shown}")
        answer = pending.pop(0)
        if answer is None:
            return None
        if answer not in labels:
            fail(f"{title} does not offer {answer}: {labels}")
        return labels.index(answer)

    real = (menu.choose, menu.message, menu.save, menu.curses.curs_set)
    menu.choose = choose
    menu.message = lambda _screen, title, lines, _target: shown.append((title, list(lines)))
    menu.save = lambda _path, _values: None
    menu.curses.curs_set = lambda _visibility: None
    try:
        function(_FakeScreen(), values)
    finally:
        menu.choose, menu.message, menu.save, menu.curses.curs_set = real
    return shown


def check_i386_and_retro() -> None:
    """WS232 (2026-10-14 user): the CPU i386 with the boards PC-9800 and
    PC/AT, and Retro right after Base in the main menu ("Retroが一番上に来て
    ます。Baseの下にしてください。", "全部の CPU で Base の下") with Xzed,
    zwm, zshell and zterm on every CPU.  On i386 the Desktop is left out by itself (2026-10-14
    user: "i386 では Desktop を自動で外す"): its entry is not shown and its
    programs are not written, and switching back to x86_64 writes them
    again.  The configurations the menu writes for i386 PC/AT and PC-9800,
    the defaults as they are and with Retro, are then resolved by make -n."""
    values = menu.defaults()
    shown = walk(menu.select_cpu_board, ["CPU: x86_64", "i386", "Back"], values)
    if shown[1] != ("CPU", ["x86_64", "arm64", "i386", "RISC-V 64", "RISC-V 32"]):
        fail(f"the CPU menu is {shown[1]}")
    if values["ZEDBSD_PLATFORM"] != "pc98" or values["ZEDBSD_VARIANT"] != "default":
        fail(f"choosing i386 selected {values['ZEDBSD_PLATFORM']}/{values['ZEDBSD_VARIANT']}")
    shown = walk(menu.select_cpu_board, ["Board: PC-9800", "PC/AT", "Back"], values)
    if shown[1] != ("Board", ["PC-9800", "PC/AT"]):
        fail(f"the i386 Board menu is {shown[1]}")
    if values["ZEDBSD_PLATFORM"] != "i386":
        fail(f"choosing PC/AT selected {values['ZEDBSD_PLATFORM']}")
    shown = walk(menu.select_cpu_board, ["CPU: i386", "i386", "Back"], values)
    if values["ZEDBSD_PLATFORM"] != "i386":
        fail("choosing i386 again changed the board")
    shown = walk(menu.select_cpu_board, ["Board: PC/AT", "PC-9800", "Back"], values)
    if values["ZEDBSD_PLATFORM"] != "pc98":
        fail(f"choosing PC-9800 selected {values['ZEDBSD_PLATFORM']}")
    other = menu.defaults()
    shown = walk(menu.select_cpu_board, ["Board: UEFI + ACPI", "Back"], other)
    if shown[1][0] != "Board" or "The board follows the CPU." not in shown[1][1]:
        fail(f"x86_64's one board was offered as a choice: {shown[1]}")
    # RISC-V 64 boots on two boards (WS256 p025): its first board, the
    # emulator's virt, is chosen with the CPU, and the VisionFive 2 is
    # offered beside it under Board.
    riscv = menu.defaults()
    walk(menu.select_cpu_board, ["CPU: x86_64", "RISC-V 64", "Back"], riscv)
    if riscv["ZEDBSD_PLATFORM"] != "riscv64-virt":
        fail(f"choosing RISC-V 64 selected {riscv['ZEDBSD_PLATFORM']}")
    shown = walk(menu.select_cpu_board,
                 ["Board: QEMU virt", "StarFive VisionFive 2", "Back"], riscv)
    if shown[1] != ("Board", ["QEMU virt", "StarFive VisionFive 2"]):
        fail(f"the RISC-V 64 Board menu is {shown[1]}")
    if riscv["ZEDBSD_PLATFORM"] != "visionfive2":
        fail(f"choosing StarFive VisionFive 2 selected {riscv['ZEDBSD_PLATFORM']}")

    for platform in ("amd64", "rpi4", "i386", "pc98"):
        values = menu.defaults()
        values["ZEDBSD_PLATFORM"] = platform
        menu.normalize(values)
        values["ZEDBSD_USER_PROGRAMS"].difference_update(menu.RETRO_PROGRAMS)
        shown = walk(lambda screen, v: menu.tui(screen, v, Path("/nonexistent")),
                     ["Retro", "[ ] Xzed", "[ ] zwm", "[ ] zshell", "[ ] zterm",
                      "Back", "Exit"], values)
        main_menu = shown[0][1]
        if main_menu.index("Retro") != main_menu.index("Base") + 1:
            fail(f"Retro is not right after Base in the main menu on {platform}: {main_menu}")
        if shown[-2] != ("Retro", ["[*] Xzed", "[*] zwm", "[*] zshell", "[*] zterm", "Back"]):
            fail(f"the Retro menu on {platform} is {shown[1]}, then {shown[-2]}")
        if not set(menu.RETRO_PROGRAMS) <= values["ZEDBSD_USER_PROGRAMS"]:
            fail(f"Retro did not select its programs on {platform}")
        walk(lambda screen, v: menu.edit_programs(screen, v, menu.retro_rows(v), "Retro"),
             ["[*] zterm", "Back"], values)
        if "zterm" in values["ZEDBSD_USER_PROGRAMS"]:
            fail(f"Retro did not turn zterm off on {platform}")
    if not menu.in_menu("zterm", "x11-applications") or menu.in_menu("zgears", "x11-applications"):
        fail("the menu shows the wrong X11 programs")

    desktop = {row[0] for row in USER_PROGRAM_ROWS if row[4] in menu.DESKTOP_GROUPS}
    desktop_packages = {row[5] for row in USER_PROGRAM_ROWS
                        if row[4] in menu.DESKTOP_GROUPS and row[5]}
    needing_desktop = {row[0] for row in USER_PROGRAM_ROWS
                       if desktop_packages.intersection(row[6].split())}
    for platform, cpu, shows in (("amd64", "x86_64", True), ("rpi4", "arm64", True),
                                 ("i386", "i386", False), ("pc98", "i386", False)):
        values = menu.defaults()
        values["ZEDBSD_PLATFORM"] = platform
        menu.normalize(values)
        shown = walk(lambda screen, v: menu.tui(screen, v, Path("/nonexistent")),
                     ["Exit"], values)
        if ("Desktop" in shown[0][1]) != shows:
            fail(f"the main menu on {platform} ({cpu}) is {shown[0][1]}")

    directory = scratch_directory("i386")

    def written(path: Path) -> list[str]:
        text = path.read_text(encoding="utf-8")
        return next(line for line in text.splitlines()
                    if line.startswith("ZEDBSD_USER_PROGRAMS := ")).split()[2:]

    def resolves(path: Path, platform: str) -> None:
        result = subprocess.run(
            ["make", "--no-print-directory", "-n", f"ZEDBSD_CONFIG={path}"],
            cwd=REPO, check=False, text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        if result.returncode != 0:
            fail(f"make -n did not resolve the {platform} config.mk "
                 f"({path}): {result.stdout[-2000:]}")

    # The defaults with only the CPU changed to i386 (its first board,
    # PC-9800), saved as they are, and then back to x86_64.
    values = menu.defaults()
    walk(menu.select_cpu_board, ["CPU: x86_64", "i386", "Back"], values)
    path = directory / "config-default-pc98.mk"
    menu.save(path, values)
    programs = written(path)
    if set(programs) & (desktop | needing_desktop):
        fail(f"the default i386 config.mk writes the Desktop: "
             f"{sorted(set(programs) & (desktop | needing_desktop))}")
    resolves(path, "pc98")
    walk(menu.select_cpu_board, ["CPU: i386", "x86_64", "Back"], values)
    path = directory / "config-default-amd64.mk"
    menu.save(path, values)
    expected = {row[0] for row in USER_PROGRAM_ROWS
                if row[4] in menu.DESKTOP_GROUPS and row[3] == "y"}
    if not expected <= set(written(path)):
        fail(f"back on x86_64 the Desktop's defaults are not written: "
             f"{sorted(expected - set(written(path)))}")

    for platform, board in (("i386", "pcat"), ("pc98", "pc98")):
        values = menu.defaults()
        values["ZEDBSD_PLATFORM"] = platform
        values["ZEDBSD_USER_PROGRAMS"].update(menu.RETRO_PROGRAMS)
        path = directory / f"config-{platform}.mk"
        menu.save(path, values)
        text = path.read_text(encoding="utf-8")
        programs = written(path)
        if not set(menu.RETRO_PROGRAMS) <= set(programs):
            fail(f"the {platform} config.mk does not build Retro: {programs}")
        if set(programs) & desktop:
            fail(f"the {platform} config.mk writes the Desktop: {sorted(set(programs) & desktop)}")
        for assignment in ("ZEDBSD_ARCHITECTURE := i386", f"ZEDBSD_BOARD := {board}",
                           "CONFIG_DRIVER_GRAPHICS := y"):
            if assignment not in text:
                fail(f"the {platform} config.mk omits {assignment}")
        resolves(path, platform)


def check_platform_drivers() -> None:
    """WS232 p004 (2026-10-14 user: "WiFiとBluetoothのドライバですが、
    プラットフォームによって選べるドライバが変わらない実装にしてください。
    Platformドライバだけ変えます。また、PiのドライバはPlatformにまとめて
    いいです。", "PiのGPUとPlatformドライバにSDも入れましょう。Piの
    プラットフォームドライバはコンフィグですべて有効にしておいてください。"):
    Drivers has Bluetooth right after WiFi; every category but Platform is
    the same on every CPU; Platform holds the chosen board's drivers alone,
    the always-built ones too (p003: every PC-98 driver), and choosing the
    Raspberry Pi 4 turns its Platform drivers on, VCHIQ and Vulkan Video
    too (2026-10-15 user: the Pi 4 video code is on by default)."""
    categories = ["Disk", "Input", "GPU", "Audio", "Ethernet", "WiFi",
                  "Bluetooth", "Camera", "USB", "Platform"]
    platforms = ("amd64", "i386", "pc98", "rpi4", "odroid-c5", "sun4u", "x68k")
    shared = None
    for platform in platforms:
        groups = menu.driver_groups(platform)
        if [title for title, _rows in groups] != categories:
            fail(f"the Drivers of {platform} are {[title for title, _rows in groups]}")
        lists = [(title, [row[0] for row in rows]) for title, rows in groups
                 if title != "Platform"]
        if shared is None:
            shared = lists
        elif lists != shared:
            fail(f"the Drivers of {platform} differ outside Platform: {lists}")
        catalog = [row for path in menu.architecture_files()
                   for row in menu.read_rows(path, 6)
                   if row[3] != "*" and menu.applies(row[3], platform)]
        if dict(groups)["Platform"] != catalog:
            fail(f"the Platform of {platform} is {dict(groups)['Platform']}")
    keys = {key for _title, listed in shared for key in listed}
    if not {"CONFIG_DRIVER_PCI_INTEL_AX211", "CONFIG_DRIVER_USB_RTL8822BU"} <= \
            set(dict(shared)["WiFi"]) or dict(shared)["Bluetooth"] != ["CONFIG_DRIVER_USB_BT"]:
        fail(f"WiFi or Bluetooth lists {dict(shared)}")
    if any(key.startswith("CONFIG_DRIVER_BCM2711_") or key in (
            "CONFIG_DRIVER_GRAPHICS", "CONFIG_DRIVER_LGY98", "CONFIG_DRIVER_ACPI")
           for key in keys):
        fail(f"a board's driver is outside Platform: {shared}")
    pi = [row[2] for row in dict(menu.driver_groups("rpi4"))["Platform"]]
    for label in ("BCM2711 SDHCI controller", "BCM2711 GENET: Raspberry Pi 4 onboard Ethernet",
                  "BCM43455 SDIO: Raspberry Pi 4 onboard WiFi",
                  "BCM43455 Bluetooth: Raspberry Pi 4 onboard (PL011 UART)",
                  "BCM2711 PWM audio: Raspberry Pi 4 headphone jack",
                  "BCM2711 GPU: HVS display and V3D (experimental)"):
        if label not in pi:
            fail(f"the Raspberry Pi 4's Platform has no {label}: {pi}")

    # The PC-9800 (p003): its drivers, a fixed one chosen, the LGY-98 toggled.
    fixed = [row[2] for row in menu.read_rows(
        menu.CONFIG_DIR / "drivers" / "architecture" / "pc98.drivers", 6)
        if row[1] == "fixed"]
    pc98 = [f"[*] {label}" for label in fixed] + [
        "[*] Cirrus Core Graph graphics", "[ ] LGY-98 Ethernet adapter"]
    if "PC-98 bus mouse" not in fixed:
        fail(f"pc98.drivers does not name the bus mouse: {fixed}")
    values = menu.defaults()
    menu.choose_platform(values, "pc98")
    values.update({"CONFIG_DRIVER_GRAPHICS": "y", "CONFIG_DRIVER_LGY98": "n"})
    shown = walk(menu.drivers_menu,
                 ["Platform", "[ ] LGY-98 Ethernet adapter", "[*] PC-98 bus mouse",
                  "Back", "Back"], values)
    if shown[0] != ("Drivers", categories + ["Back"]):
        fail(f"Drivers shows {shown[0]}")
    if shown[1] != ("Platform", pc98 + ["Back"]):
        fail(f"the PC-9800's Platform is {shown[1]}")
    if values["CONFIG_DRIVER_LGY98"] != "y":
        fail("Platform did not turn the LGY-98 on")
    if shown[3] != ("Platform", ["PC-98 bus mouse", "", "is always built into the kernel."]):
        fail(f"choosing a fixed driver showed {shown[3]}")
    if "-" in values:
        fail("choosing a fixed driver set a value")

    # The Raspberry Pi 4 chosen from the x86_64 default, and its config.mk.
    values = menu.defaults()
    shown = walk(menu.select_cpu_board, ["CPU: x86_64", "arm64", "Back"], values)
    if values["ZEDBSD_PLATFORM"] != "rpi4":
        fail(f"choosing arm64 made {values['ZEDBSD_PLATFORM']}")
    expected = {"CONFIG_DRIVER_BCM2711_GENET": "y", "CONFIG_DRIVER_BCM2711_WIFI": "y",
                "CONFIG_DRIVER_BCM2711_BT": "y", "CONFIG_DRIVER_BCM2711_AUDIO": "y",
                "CONFIG_DRIVER_BCM2711_GPU": "y", "CONFIG_DRIVER_BCM2711_VCHIQ": "y",
                "CONFIG_DRIVER_BCM2711_VIDEO": "y", "CONFIG_DRIVER_USB_BT": "y"}
    for key, value in expected.items():
        if values.get(key) != value:
            fail(f"the Raspberry Pi 4 chosen from x86_64 has {key}={values.get(key)}")
    # The CI image's file names every Platform driver; VCHIQ and Vulkan Video
    # (y since 2026-10-14) and the built-in Bluetooth (n since 2026-10-15, the
    # user took it out of main) are the user's choice there, so only their
    # lines are checked.
    ci = menu.load(menu.REPO / "config" / "ci" / "config-rpi4.mk")
    text = (menu.REPO / "config" / "ci" / "config-rpi4.mk").read_text(encoding="utf-8")
    for key, value in expected.items():
        if key in ("CONFIG_DRIVER_BCM2711_VCHIQ", "CONFIG_DRIVER_BCM2711_VIDEO",
                   "CONFIG_DRIVER_BCM2711_BT"):
            if f"{key} := {ci.get(key)}" not in text:
                fail(f"config/ci/config-rpi4.mk does not set {key}")
            continue
        if ci.get(key) != value or f"{key} := {value}" not in text:
            fail(f"config/ci/config-rpi4.mk does not set {key} := {value}")


def check_driver_firmware() -> None:
    """WS232 p004 (2026-10-14 user: "menuconfig で Pi を選んだ時、WiFi と
    Bluetooth の firmware（bcm43455・brcmbt）も自動で選びますか？-->yes",
    "Intel GPUのドライバも、選択したらファームウェアも選択されるように
    お願いします。そのほかのドライバも、ファームウェアを選択してください。"):
    a driver turned on selects its firmware, in Drivers and as a board's
    default; turned off, its firmware goes unless another driver needs it;
    a package chosen by hand stays; and every CI image but the user's
    server one selects the firmware of its drivers."""
    table = menu.driver_firmware_table()
    drivers = {row[0] for row in menu.driver_rows()}
    boards = {item[0] for item in menu.PLATFORMS}
    firmware = {row[0] for row in USER_PROGRAM_ROWS if row[4] == "firmware"}
    for key, packages in table.items():
        if key not in drivers and key not in boards:
            fail(f"config/drivers/firmware.list names no driver or platform {key}")
        for package in packages:
            if package not in firmware:
                fail(f"config/drivers/firmware.list names no firmware package {package}")
    unclaimed = firmware - {package for packages in table.values() for package in packages}
    if unclaimed:
        fail(f"no driver in config/drivers/firmware.list needs {sorted(unclaimed)}")

    def selected(values: dict[str, object]) -> set[str]:
        return set(values["ZEDBSD_USER_PROGRAMS"]) & firmware

    # The x86_64 default: the i915, nvrtx (on by default since 2026-10-15)
    # and USB Bluetooth drivers are on.
    values = menu.defaults()
    if selected(values) != {"i915-firmware", "intelbt-firmware", "nvidia-gsp-firmware"}:
        fail(f"the x86_64 default selects {sorted(selected(values))}")

    # Drivers > GPU: the NVIDIA driver brings its firmware and takes it away; so does i915.
    gpu = {row[0]: row[2] for row in dict(menu.driver_groups("amd64"))["GPU"]}
    nvrtx = gpu["CONFIG_DRIVER_PCI_NVRTX"]
    i915 = gpu["CONFIG_DRIVER_PCI_I915"]
    values["ZEDBSD_USER_PROGRAMS"].add("rtl8822b-firmware")
    # Both start on (nvrtx by default since 2026-10-15).
    walk(menu.drivers_menu, ["GPU", f"[*] {nvrtx}", f"[*] {i915}", "Back", "Back"], values)
    if selected(values) != {"intelbt-firmware", "rtl8822b-firmware"}:
        fail(f"turning the GPU drivers off left {sorted(selected(values))}")
    walk(menu.drivers_menu, ["GPU", f"[ ] {nvrtx}", "Back", "Back"], values)
    if "nvidia-gsp-firmware" not in values["ZEDBSD_USER_PROGRAMS"]:
        fail("turning the NVIDIA driver on did not select nvidia-gsp-firmware")
    walk(menu.drivers_menu, ["GPU", f"[*] {nvrtx}", "Back", "Back"], values)
    if "nvidia-gsp-firmware" in values["ZEDBSD_USER_PROGRAMS"]:
        fail("turning the NVIDIA driver off left nvidia-gsp-firmware")
    walk(menu.drivers_menu, ["GPU", f"[ ] {i915}", "Back", "Back"], values)
    if "i915-firmware" not in values["ZEDBSD_USER_PROGRAMS"]:
        fail("turning the i915 driver on did not select i915-firmware")

    # The Raspberry Pi 4 from x86_64: its WiFi and Bluetooth firmware come, i915's goes.
    walk(menu.select_cpu_board, ["CPU: x86_64", "arm64", "Back"], values)
    if selected(values) != {"bcm43455-firmware", "brcmbt-firmware", "intelbt-firmware",
                            "rtl8822b-firmware"}:
        fail(f"the Raspberry Pi 4 chosen from x86_64 selects {sorted(selected(values))}")
    pi = {row[0]: row[2] for row in dict(menu.driver_groups("rpi4"))["Platform"]}
    walk(menu.drivers_menu, ["Platform", f"[*] {pi['CONFIG_DRIVER_BCM2711_BT']}", "Back", "Back"],
         values)
    if "brcmbt-firmware" in values["ZEDBSD_USER_PROGRAMS"]:
        fail("turning the Pi's Bluetooth off kept brcmbt-firmware")
    walk(menu.select_cpu_board, ["CPU: arm64", "x86_64", "Back"], values)
    if selected(values) != {"i915-firmware", "intelbt-firmware", "rtl8822b-firmware"}:
        fail(f"x86_64 chosen back from the Pi selects {sorted(selected(values))}")

    # The ODROID-C5 (2026-10-30 user: "ODROID-C5のBSPを選ぶと、Amlogicのこのチップの
    # ファームウェアが自動で依存関係で有効になるようにしてください。"): the board
    # brings its bootloader and its Mali driver the GPU's firmware; turning the
    # driver off takes the GPU's firmware, not the bootloader; another board
    # takes both.
    walk(menu.select_cpu_board, ["CPU: x86_64", "arm64", "Board: Raspberry Pi 4",
                                 "ODROID-C5", "Back"], values)
    if values["ZEDBSD_PLATFORM"] != "odroid-c5":
        fail(f"choosing the ODROID-C5 made {values['ZEDBSD_PLATFORM']}")
    if not {"amlogic-s7d-bootloader", "mali-csf-firmware"} <= selected(values) or \
            selected(values) & {"bcm43455-firmware", "brcmbt-firmware", "i915-firmware"}:
        fail(f"the ODROID-C5 selects {sorted(selected(values))}")
    c5 = {row[0]: row[2] for row in dict(menu.driver_groups("odroid-c5"))["Platform"]}
    walk(menu.drivers_menu, ["Platform", f"[*] {c5['CONFIG_DRIVER_MALI_GPU']}", "Back", "Back"],
         values)
    if "mali-csf-firmware" in values["ZEDBSD_USER_PROGRAMS"] or \
            "amlogic-s7d-bootloader" not in values["ZEDBSD_USER_PROGRAMS"]:
        fail(f"the ODROID-C5 without its Mali driver selects {sorted(selected(values))}")
    walk(menu.select_cpu_board, ["Board: ODROID-C5", "Raspberry Pi 4", "Back"],
         values)
    if selected(values) & {"amlogic-s7d-bootloader", "mali-csf-firmware"}:
        fail(f"the Raspberry Pi 4 chosen from the ODROID-C5 selects {sorted(selected(values))}")
    for name, mali in (("config-odroid-c5.mk", "n"), ("config-odroid-c5-keiland.mk", "y")):
        ci = menu.load(menu.REPO / "config" / "ci" / name)
        if ci["ZEDBSD_PLATFORM"] != "odroid-c5" or ci.get("CONFIG_DRIVER_MALI_GPU") != mali or \
                "amlogic-s7d-bootloader" not in ci["ZEDBSD_USER_PROGRAMS"]:
            fail(f"{name} loads as {ci['ZEDBSD_PLATFORM']}, Mali {ci.get('CONFIG_DRIVER_MALI_GPU')}")

    # Every CI image selects the firmware of the drivers it builds (the server one is the user's).
    for path in sorted((menu.REPO / "config" / "ci").glob("config-*.mk")):
        if path.name == "config-amd64-server.mk":
            continue
        ci = menu.load(path)
        missing = menu.driver_firmware(ci, str(ci["ZEDBSD_PLATFORM"])) - set(ci["ZEDBSD_USER_PROGRAMS"])
        if missing:
            fail(f"{path.name} builds drivers without {sorted(missing)}")


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
            "broken:\nfirmware-licenses-pending:\n\t@true\n", encoding="utf-8")
        values = {"ZEDBSD_PLATFORM": "amd64"}
        menu.build_boot_image(None, values, tree / "config.mk")
        if not results or results[-1][0] != "Build succeeded.":
            fail(f"the stand-in build did not succeed: {results}")
        counted = [entry for entry in drawn if entry[2] in ("first", "second", "disk-image")]
        if len(counted) != 3 or counted[-1][0] != 3 or counted[-1][1] != 3:
            fail(f"the progress counted {drawn}")
        (tree / "Makefile").write_text("disk-image:\n\t@echo the step that failed; false\n"
                                       "firmware-licenses-pending:\n\t@true\n", encoding="utf-8")
        menu.build_boot_image(None, values, tree / "config.mk")
        if not results[-1][0].startswith("Build failed") or "the step that failed" not in results[-1]:
            fail(f"a failed build did not show the log's end: {results[-1]}")
    finally:
        menu.REPO, menu.choose, menu.save, menu.draw_progress, menu.message = real


class _TextScreen:
    """A screen of a given size drawn into an array of characters, with the
    attributes of each cell and the keys it answers with, for the license
    agreement (WS233 p002)."""

    def __init__(self, height: int, width: int, keys: list[int]):
        self.height, self.width = height, width
        self.keys = list(keys)
        self.cells = [[" "] * width for _ in range(height)]
        self.attrs = [[0] * width for _ in range(height)]
        self.frames: list[str] = []

    def getmaxyx(self):
        return self.height, self.width

    def keypad(self, _flag):
        return None

    def erase(self):
        self.cells = [[" "] * self.width for _ in range(self.height)]
        self.attrs = [[0] * self.width for _ in range(self.height)]

    def addnstr(self, y, x, text, length, attr=0):
        if not 0 <= y < self.height or not 0 <= x < self.width:
            raise menu.curses.error("out of the screen")
        for offset, character in enumerate(text[:length]):
            if x + offset >= self.width:
                break
            self.cells[y][x + offset] = character
            self.attrs[y][x + offset] = attr

    def refresh(self):
        self.frames.append(self.text())

    def getch(self):
        if not self.keys:
            fail("the license screen asked for more keys than the test gave")
        return self.keys.pop(0)

    def text(self) -> str:
        return "\n".join("".join(row).rstrip() for row in self.cells)

    def attr_of(self, needle: str) -> int:
        for y, row in enumerate(self.cells):
            line = "".join(row)
            x = line.find(needle)
            if x >= 0:
                return self.attrs[y][x]
        fail(f"{needle} is not on the screen")
        return 0


def check_firmware_licenses() -> None:
    """WS233 p002 (2026-10-14 user: the firmware licenses in the build menu,
    scrolled and looking like a legal document): make lists the licenses the
    build would ask about without asking, an acceptance is recorded by
    license-agree.sh in the same line the interactive question writes, a
    declined license starts no build, the build's make never reads the
    terminal, and the agreement screen at 80x24, 120x40 and too small."""
    directory = scratch_directory("licenses")
    config = REPO / "config" / "ci" / "config-rpi4.mk"
    roots = {name: directory / "firmware" / name
             for name in ("rtl8822b", "brcmbt", "bcm43455", "rpi4-boot")}
    extra = (f"RTL8822B_FIRMWARE_CACHE_ROOT={roots['rtl8822b']}",
             f"BRCMBT_FIRMWARE_CACHE_ROOT={roots['brcmbt']}",
             f"BCM43455_FIRMWARE_CACHE_ROOT={roots['bcm43455']}",
             f"RPI4_BOOT_FIRMWARE_CACHE_ROOT={roots['rpi4-boot']}")
    pending = menu.firmware_licenses_pending(config, extra)
    if isinstance(pending, str):
        fail(f"make firmware-licenses-pending failed: {pending}")
    names = [entry["package"] for entry in pending]
    if sorted(names) != ["bcm43455-firmware", "brcmbt-firmware", "rpi4-boot-firmware",
                         "rtl8822b-firmware"]:
        fail(f"the pending licenses of the Raspberry Pi 4 are {names}")
    for entry in pending:
        data = Path(entry["text"]).read_bytes()
        if len(data) != int(entry["size"]) or \
                __import__("hashlib").sha256(data).hexdigest() != entry["sha256"]:
            fail(f"the license text of {entry['package']} is not the declared one")
        if not Path(entry["notice"]).is_file():
            fail(f"the notice of {entry['package']} is missing")
        # The screen folds paragraphs, but every word of the license stays, in order.
        text = data.decode("utf-8")
        if " ".join(menu.license_paragraphs(text)).split() != text.split():
            fail(f"the screen's license of {entry['package']} is not the license's words")

    # The acceptance the menu records is the line the interactive question writes.
    entry = next(item for item in pending if item["package"] == "rpi4-boot-firmware")
    error = menu.accept_license(entry)
    if error is not None:
        fail(f"accepting {entry['package']} failed: {error}")
    asked_root = directory / "asked"
    asked_root.mkdir()
    answered = subprocess.run(
        ["sh", str(menu.license_script()), entry["package"], str(asked_root / "license-accepted"),
         entry["notice"], entry["name"], entry["size"], entry["sha256"], "",
         "file://" + entry["text"], "curl", "--fail", "--silent", "--show-error"],
        cwd=REPO, input="yes\n", text=True, check=False,
        stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if answered.returncode != 0:
        fail(f"the interactive question did not accept: {answered.stderr}")
    recorded = Path(entry["record"]).read_text(encoding="utf-8")
    if recorded != (asked_root / "license-accepted").read_text(encoding="utf-8") or \
            recorded != f"{entry['sha256']}  {entry['name']}\n":
        fail(f"the menu's record {recorded!r} differs from the question's")
    again = menu.firmware_licenses_pending(config, extra)
    if isinstance(again, str) or "rpi4-boot-firmware" in [item["package"] for item in again] \
            or len(again) != 3:
        fail(f"an accepted license is still pending: {again}")
    # A text that is not the one shown is not recorded.
    forged = dict(entry, sha256="0" * 64, record=str(directory / "forged-accepted"))
    if menu.accept_license(forged) is None or Path(forged["record"]).exists():
        fail("an acceptance of a text never shown was recorded")

    # The agreement at 80x24 and 120x40: Accept stays dim until the end has
    # been shown; End reaches it and Enter on Accept (after Tab) accepts.
    entry = next(item for item in again if item["package"] == "brcmbt-firmware")
    frames: dict[str, str] = {}
    for height, width in ((24, 80), (40, 120)):
        screen = _TextScreen(height, width, [menu.curses.KEY_DOWN, ord("a"), 10])
        if menu.license_screen(screen, entry, 2, 4):
            fail(f"{height}x{width}: Accept or Enter agreed before the end was shown")
        first = screen.frames[0]
        for needle in ("Firmware License Agreement", "brcmbt-firmware", "License 2 of 4",
                       "1. THE FIRMWARE", "Lines 1-", "[ Accept ]", "[ Decline ]",
                       "Scroll to the end of the license to enable Accept."):
            if needle not in first:
                fail(f"{height}x{width}: the agreement does not show {needle!r}:\n{first}")
        screen = _TextScreen(height, width, [menu.curses.KEY_END, 9, 10])
        accepted = menu.license_screen(screen, entry, 2, 4)
        if not accepted or "(100%)" not in screen.frames[-1] or \
                "Tab switches, Enter confirms, D declines." not in screen.frames[-1]:
            fail(f"{height}x{width}: End, Tab and Enter did not accept:\n{screen.frames[-1]}")
        frames[f"{width}x{height}-top"] = first
        frames[f"{width}x{height}-end"] = screen.frames[-2]
        dim = _TextScreen(height, width, [ord("d")])
        menu.license_screen(dim, entry, 2, 4)
        if not dim.attr_of("[ Accept ]") & menu.curses.A_DIM:
            fail(f"{height}x{width}: Accept is not dim before the end")
        for line in frames[f"{width}x{height}-top"].splitlines():
            if len(line) > width:
                fail(f"{height}x{width}: a line runs off the screen: {line}")
    small = _TextScreen(10, 30, [ord("a"), 10, ord("d")])
    if menu.license_screen(small, entry) or "too small" not in small.frames[0]:
        fail(f"a too small terminal did not say so or accepted:\n{small.frames[0]}")
    frames["30x10"] = small.frames[0]
    for name, text in frames.items():
        (directory / f"screen-{name}.txt").write_text(text + "\n", encoding="utf-8")

    # Declining starts no build and says what to do; the build's make reads no terminal.
    started: list[dict] = []
    said: list[list[str]] = []

    class _Process:
        stdout: list[str] = []

        def wait(self):
            return 0

    def popen(arguments, **options):
        started.append(dict(options, arguments=arguments))
        return _Process()

    real = (menu.choose, menu.save, menu.message, menu.license_screen, menu.firmware_licenses_pending,
            menu.accept_license, menu.draw_progress, menu.subprocess.Popen, menu.subprocess.run)
    runs: list[dict] = []
    menu.choose = lambda _screen, _title, _labels, _target, _selected=0: 0
    menu.save = lambda _path, _values: None
    menu.message = lambda _screen, _title, lines, _target: said.append(list(lines))
    menu.draw_progress = lambda *_arguments: None
    menu.firmware_licenses_pending = lambda _config, _extra=(): [dict(entry), dict(pending[0])]
    menu.accept_license = lambda _entry: fail("a declined license was recorded")
    menu.license_screen = lambda _screen, _entry, _number=1, _count=1: False
    try:
        menu.build_boot_image(None, {"ZEDBSD_PLATFORM": "rpi4"}, directory / "config.mk")
        if started or not said or "brcmbt-firmware" not in said[-1][0] or \
                not any("Firmware menu" in line for line in said[-1]):
            fail(f"a declined license started the build or said {said}")
        menu.firmware_licenses_pending = lambda _config, _extra=(): []

        def run(arguments, **options):
            runs.append(dict(options, arguments=arguments))
            return subprocess.CompletedProcess(arguments, 0, "", "")

        menu.subprocess.Popen = popen
        menu.subprocess.run = run
        menu.build_boot_image(None, {"ZEDBSD_PLATFORM": "rpi4"}, directory / "config.mk")
    finally:
        (menu.choose, menu.save, menu.message, menu.license_screen, menu.firmware_licenses_pending,
         menu.accept_license, menu.draw_progress, menu.subprocess.Popen, menu.subprocess.run) = real
    if len(started) != 1 or started[0].get("stdin") is not subprocess.DEVNULL:
        fail(f"the build's make is not run with its input closed: {started}")
    if not runs or any(item.get("stdin") is not subprocess.DEVNULL for item in runs):
        fail(f"the build's counting make is not run with its input closed: {runs}")
    print(f"MAC-T001 firmware licenses: PASS (screens in {directory})")


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
    check_i386_and_retro()
    check_platform_drivers()
    check_driver_firmware()
    check_build_progress()
    check_firmware_licenses()

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
          "package requirements resolved, the WS193 menu, "
          "i386 PC/AT and PC-9800 with Retro after Base and without the Desktop, "
          "the board drivers under Platform alone, Bluetooth after WiFi, "
          "the drivers' firmware selected with them)")


if __name__ == "__main__":
    main()
