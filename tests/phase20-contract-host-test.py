#!/usr/bin/env python3
"""Focused source/configuration contract checks for Phase 20."""

import argparse
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parent.parent


def read(path):
    return (ROOT / path).read_text()


def parse_data(path):
    values = {}
    for number, raw in enumerate(read(path).splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            raise AssertionError(f"{path}:{number}: missing equals")
        key, value = line.split("=", 1)
        if not re.fullmatch(r"[A-Za-z0-9_-]+", key):
            raise AssertionError(f"{path}:{number}: invalid key")
        if value[:1] in ('"', "'"):
            assert value[-1:] == value[:1]
            value = value[1:-1]
        assert key not in values
        values[key] = value
    return values


def check_init():
    source = read("userland/base/init/main.c")
    networkd = parse_data("userland/base/init/services/networkd")
    net = parse_data("userland/base/init/services/net")
    assert networkd["notify-fd3"] == "on"
    assert 1 <= int(networkd["notify-timeout"]) <= 300
    assert net["type"] == "oneshot" and net["arguments"] == "boot"
    assert net["after"] == "networkd" and net["requires"] == "networkd"
    for token in ("SERVICE_COMPLETED", "SERVICE_SKIPPED", "READY", "FAIL ",
                  "ZEDBSD_NOTIFY_FD", "pipe2", "DEPENDENCIES_SKIP"):
        assert token in source


def check_net_config():
    values = parse_data("userland/base/etc/rc.conf")
    assert values["networkd_enable"] == "YES"
    assert values["net_enable"] == "YES"
    assert values["net_lo0"] == (
        "static ipv4 127.0.0.1 netmask 255.0.0.0"
    )
    assert 1 <= int(values["net_dhcptimeout"]) <= 3600
    source = read("userland/base/net/main.c")
    for token in ("net_auto", "net_dhcptimeout", "net_defaultroute",
                  "net_dns", "unsupported or invalid", "duplicate interface"):
        assert token in source
    assert "system(" not in source and "popen(" not in source


def check_protocol():
    header = read("userland/base/net/protocol.h")
    daemon = read("userland/base/networkd/main.c")
    client = read("userland/base/net/main.c")
    assert '#define NETWORKD_PROTOCOL_VERSION "V1"' in header
    for operation in ("SHOW", "UP", "DOWN", "STATIC", "DHCP",
                      "DEFAULTROUTE", "DNS"):
        assert f'"{operation}"' in daemon
        assert f'"{operation}"' in client
    for command in ("/sbin/ifconfig", "/sbin/route", "/sbin/dhcpc"):
        assert command in daemon
    assert "/sbin/dhcpcd" not in daemon
    assert "fork()" in daemon and "execv(" in daemon


def check_dhcp():
    assert (ROOT / "userland/base/dhcpc/Makefile").is_file()
    assert (ROOT / "userland/base/dhcpc/main.c").is_file()
    assert not (ROOT / "userland/base/dhcpcd").exists()
    makefile = read("userland/base/dhcpc/Makefile")
    source = read("userland/base/dhcpc/main.c")
    assert "ZEDBSD_USERLAND_PACKAGE,dhcpc,dhcpc" in makefile
    assert "usage: dhcpc " in source
    assert "dhcpcd:" not in source
    for forbidden in ("daemon(", "setsid(", "pidfile", "fork("):
        assert forbidden not in source
    assert "O_EXCL" in source and "fsync(" in source and "rename(" in source


CHECKS = {
    "init": check_init,
    "net-config": check_net_config,
    "protocol": check_protocol,
    "dhcp": check_dhcp,
}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("section", choices=CHECKS)
    args = parser.parse_args()
    CHECKS[args.section]()
    print(f"Phase 20 {args.section} host test: PASS")


if __name__ == "__main__":
    main()
