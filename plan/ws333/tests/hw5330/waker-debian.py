#!/usr/bin/env python3
# ws333: wakes every CPU of the 5330's Debian on a common period, the way zedBSD's periodic tick wakes each CPU
# every millisecond with the deadlines on multiples of the period, to see whether such wakeups alone keep the
# package out of its C-states.  One process per CPU, each bound to its CPU, sleeping to the next multiple of the
# period on CLOCK_MONOTONIC and doing nothing else.
#   waker-debian.py PERIOD_US SECONDS
# Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
import os
import sys
import time


def run(cpu, period, seconds):
    os.sched_setaffinity(0, {cpu})
    end = time.monotonic() + seconds
    while True:
        now = time.monotonic()
        if now >= end:
            return
        time.sleep(period - now % period)


def main():
    period = int(sys.argv[1]) / 1e6
    seconds = float(sys.argv[2])
    children = []
    for cpu in sorted(os.sched_getaffinity(0)):
        pid = os.fork()
        if pid == 0:
            run(cpu, period, seconds)
            os._exit(0)
        children.append(pid)
    for pid in children:
        os.waitpid(pid, 0)
    return 0


if __name__ == "__main__":
    sys.exit(main())
