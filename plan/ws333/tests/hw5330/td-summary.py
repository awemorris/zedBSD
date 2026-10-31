#!/usr/bin/env python3
# ws333: the summary of 5330-thermal.sh, from the kernel logs it kept (the "thermal-diag:" lines of thermal-diag.patch).
#   td-summary.py p011 BOOT IDLE PERFORMANCE PERFORMANCE-LOAD POWERSAVE-LOAD BALANCED
#   td-summary.py p011-off LOG
#   td-summary.py p012 TAG W1 W2 W3        (the i915 diagnostic's files TAG-wN-gpu0.txt and -gpu1.txt beside them)
#   td-summary.py p018 BOOT START LOAD...
#   td-summary.py p018-boost BOOT IDLE ON LOAD OFF END
#   td-summary.py hot LOG MILLICELSIUS    exit 0 when a sensor of the last dump is above MILLICELSIUS
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import re
import sys

TAG = "thermal-diag: "
BOOT_WORDS = ("cpu-x86:", "cpuperf:", "thermal:", "dell:", "A64 TIMER TICK", "A64 IDLE")
CLASS = {"0": "?", "1": "P", "2": "E"}


def lines(path):
    try:
        with open(path, errors="replace") as f:
            return [l.rstrip("\n") for l in f]
    except OSError:
        return []


def fields(text):
    return dict(re.findall(r"(\w+)=(\S+)", text))


def dumps(path):
    """The dumps of a log in order: each a dict of status, receivers, fans, meters, sensors, pkg, cpus, domains, cmd."""
    out = []
    cur = None
    for line in lines(path):
        i = line.find(TAG)
        if i < 0:
            continue
        body = line[i + len(TAG):]
        word = body.split(" ", 1)[0]
        rest = body[len(word):].strip()
        if word == "status":
            cur = {"status": fields(rest), "receivers": {}, "fans": {}, "meters": {}, "sensors": {}, "pkg": {},
                   "pkg_head": {}, "cpus": {}, "domains": {}}
            out.append(cur)
        elif word == "cmd":
            if cur is not None and rest.startswith(("td-dump", "td-idle")) and "cmd" not in cur:
                cur["cmd"] = rest
        elif cur is None:
            continue
        elif word in ("receiver", "fan", "meter", "sensor"):
            parts = rest.split(" ", 2)
            entry = fields(rest)
            entry["name"] = parts[1]
            cur[word + "s"][parts[1]] = entry
        elif word == "pkg-idle":
            f = fields(rest)
            if "state" in f:
                cur["pkg"][int(f["state"])] = int(f["ns"])
            else:
                cur["pkg_head"] = f
        elif word == "cpu":
            cur["cpus"][int(rest.split(" ", 1)[0])] = fields(rest)
        elif word == "domain":
            cur["domains"][int(rest.split(" ", 1)[0])] = fields(rest)
    return out


def commands(path):
    """The set and boost commands of a log with their answers."""
    out = []
    for line in lines(path):
        i = line.find(TAG + "cmd ")
        if i >= 0 and not re.search(r"cmd td-(dump|idle) ", line):
            out.append(line[i + len(TAG):])
    return out


def grep(path, words):
    return [l for l in lines(path) if any(w in l for w in words)]


def watts(a, b):
    """Each meter's average power between two dumps, from the core's copies."""
    out = {}
    for name, mb in b["meters"].items():
        ma = a["meters"].get(name)
        if ma is None:
            continue
        dt = int(mb["time_ns"]) - int(ma["time_ns"])
        if dt > 0:
            out[name] = (int(mb["energy_nj"]) - int(ma["energy_nj"])) / dt, dt / 1e9
    return out


def residency(a, b):
    """The package C-state residency between two dumps, in percent of the time between them."""
    dt = int(b["pkg_head"].get("time_ns", 0)) - int(a["pkg_head"].get("time_ns", 0))
    if dt <= 0:
        return {}, 0
    return {s: 100.0 * (b["pkg"][s] - a["pkg"].get(s, 0)) / dt for s in sorted(b["pkg"])}, dt / 1e9


def show_power(a, b, label):
    w = watts(a, b)
    text = ", ".join("%s %.2f W" % (n, v[0]) for n, v in sorted(w.items()))
    secs = max([v[1] for v in w.values()] or [0])
    print("%s: meters over %.1f s: %s" % (label, secs, text or "none"))
    r, secs = residency(a, b)
    if r:
        print("%s: package residency over %.1f s: %s" % (label, secs, ", ".join("PC%d %.1f%%" % (s, v) for s, v in r.items())))
    else:
        print("%s: package residency: none (%s)" % (label, b["pkg_head"].get("error", "?")))
    return w, r


def show_cpus(d, label):
    for n in sorted(d["cpus"]):
        c = d["cpus"][n]
        print("%s: cpu %2d %s core %s module %s domain %s levels %s highest %s MHz delivered %s MHz relative %s "
              "feedback %s/%s flags %s error %s" % (label, n, CLASS.get(c.get("class"), c.get("class")), c.get("core"),
                                                    c.get("module"), c.get("domain"), c.get("levels"), c.get("highest_mhz"),
                                                    c.get("delivered_mhz"), c.get("relative"), c.get("available"),
                                                    c.get("efficiency"), c.get("flags"), c.get("error")))


def delivered(d, label):
    by = {}
    for c in d["cpus"].values():
        by.setdefault(CLASS.get(c.get("class"), "?"), []).append(int(c.get("delivered_mhz", 0)))
    print("%s: delivered MHz %s" % (label, "; ".join("%s %s (mean %.0f)" % (k, v, sum(v) / len(v)) for k, v in sorted(by.items()))))


def show_domains(d, label):
    for n in sorted(d["domains"]):
        x = d["domains"][n]
        print("%s: domain %d cpus %s request %s limit %s flags %s" % (label, n, x.get("cpus"), x.get("request"),
                                                                    x.get("limit"), x.get("flags")))


def show_thermal(d, label):
    for name, r in d["receivers"].items():
        print("%s: receiver %s error %s flags %s generation %s %s ms" % (label, name, r.get("error"), r.get("flags"),
                                                                       r.get("gen"), r.get("ms")))
    for name, f in d["fans"].items():
        print("%s: fan %s rpm %s level %s boost_count %s boost %s copy %s" % (label, name, f.get("rpm"), f.get("level"),
                                                                             f.get("boost_count"), f.get("boost"), f.get("copy")))
    temps = ["%s %.1f C" % (n, int(s.get("mC", 0)) / 1000.0) for n, s in d["sensors"].items()]
    print("%s: sensors %s" % (label, ", ".join(temps) or "none"))


def new_lines(before, after, words):
    seen = set(grep(before, words)) if before else set()
    return [l for l in grep(after, words) if l not in seen]


def p011(boot, idle, perf, perf_load, ps_load, bal):
    for l in grep(boot, BOOT_WORDS):
        print("boot: " + l.strip())
    d = dumps(idle)
    if len(d) < 2:
        print("p011: FAIL: fewer than two dumps in the idle step")
        return
    show_power(d[-2], d[-1], "idle")
    show_cpus(d[-1], "idle")
    show_domains(d[-1], "idle")
    show_thermal(d[-1], "idle")
    prev = idle
    for path, label in ((perf, "performance"), (perf_load, "performance+load"), (ps_load, "powersave+load"), (bal, "balanced")):
        for c in commands(path):
            if c not in commands(prev):
                print("%s: %s" % (label, c))
        for l in new_lines(prev, path, ("cpu-x86:", "cpuperf:", "thermal:", "dell:")):
            print("%s: log: %s" % (label, l.strip()))
        d = dumps(path)
        if len(d) >= 2:
            show_power(d[-2], d[-1], label)
        if d:
            if label == "performance":
                show_cpus(d[-1], label)
            delivered(d[-1], label)
            show_domains(d[-1], label)
            show_thermal(d[-1], label)
            print("%s: cpu flags %s" % (label, sorted({c.get("flags") for c in d[-1]["cpus"].values()})))
        prev = path


def p011_off(log):
    for l in grep(log, BOOT_WORDS):
        print("off: " + l.strip())
    d = dumps(log)
    if d:
        print("off: status " + " ".join("%s=%s" % kv for kv in d[-1]["status"].items()))
        print("off: cpu errors %s, package residency error %s" % (sorted({c.get("error") for c in d[-1]["cpus"].values()}),
                                                                  d[-1]["pkg_head"].get("error")))


def gpu_energy(path):
    for l in lines(path):
        if "gpu=0" in l:
            f = fields(l)
            return int(f["objects_bytes"]), int(f["time_ns"])
    return None


def p012(tag, *windows):
    for l in grep(windows[0], ("A64 TIMER TICK", "A64 IDLE", "cpu-x86: family")):
        print("%s: %s" % (tag, l.strip()))
    for n, path in enumerate(windows, 1):
        d = dumps(path)
        if len(d) < 2:
            print("%s w%d: FAIL: fewer than two dumps" % (tag, n))
            continue
        show_power(d[-2], d[-1], "%s w%d" % (tag, n))
        base = path[:-4]
        a, b = gpu_energy(base + "-gpu0.txt"), gpu_energy(base + "-gpu1.txt")
        if a and b and b[1] > a[1]:
            unit = (a[0] >> 40) & 0x1f
            joules = ((b[0] - a[0]) & 0xffffffff) / float(1 << unit)
            print("%s w%d: i915 diagnostic package %.2f W over %.1f s" % (tag, n, joules / ((b[1] - a[1]) / 1e9), (b[1] - a[1]) / 1e9))
        else:
            print("%s w%d: i915 diagnostic: no reading" % (tag, n))


def p018(boot, start, *loads):
    for l in grep(boot, ("dell:", "wmi:", "WMI")):
        print("boot: " + l.strip())
    d = dumps(start)
    if d:
        show_thermal(d[-1], "start")
    prev = start
    for path in loads:
        label = os.path.basename(path)[:-4]
        for c in commands(path):
            if c not in commands(prev):
                print("%s: %s" % (label, c))
        for l in new_lines(prev, path, ("dell:", "thermal:")):
            print("%s: log: %s" % (label, l.strip()))
        ds = dumps(path)
        n = len(dumps(prev))
        for i, x in enumerate(ds[n:]):
            rpm = ", ".join("%s %s rpm" % (k, f.get("rpm")) for k, f in x["fans"].items())
            hot = max([int(s.get("mC", 0)) for s in x["sensors"].values()] or [0]) / 1000.0
            rec = ", ".join("%s e%s %sms" % (k, r.get("error"), r.get("ms")) for k, r in x["receivers"].items())
            print("%s: dump %d: %s, hottest %.1f C, receivers %s" % (label, i, rpm or "no fan", hot, rec))
        prev = path


def p018_boost(boot, idle, on, load, off, end):
    for l in grep(boot, ("dell:",)):
        print("boost boot: " + l.strip())
    prev = boot
    for path in (idle, on, load, off, end):
        label = os.path.basename(path)[:-4]
        for c in commands(path):
            if c not in commands(prev):
                print("%s: %s" % (label, c))
        for l in new_lines(prev, path, ("dell:", "thermal:")):
            print("%s: log: %s" % (label, l.strip()))
        ds = dumps(path)
        n = len(dumps(prev))
        for i, x in enumerate(ds[n:]):
            fan = "; ".join("%s rpm %s level %s boost_count %s boost %s" % (k, f.get("rpm"), f.get("level"), f.get("boost_count"),
                                                                            f.get("boost")) for k, f in x["fans"].items())
            hot = max([int(s.get("mC", 0)) for s in x["sensors"].values()] or [0]) / 1000.0
            print("%s: dump %d: %s, hottest %.1f C" % (label, i, fan or "no fan", hot))
        prev = path


def hot(log, limit):
    d = dumps(log)
    if d and any(int(s.get("mC", 0)) > int(limit) for s in d[-1]["sensors"].values()):
        sys.exit(0)
    sys.exit(1)


def main():
    mode, args = sys.argv[1], sys.argv[2:]
    {"p011": p011, "p011-off": p011_off, "p012": p012, "p018": p018, "p018-boost": p018_boost, "hot": hot}[mode](*args)


main()
