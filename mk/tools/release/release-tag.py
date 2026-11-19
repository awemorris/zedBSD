#!/usr/bin/env python3

# zedBSD
# Copyright (C) 2026 Awe Morris
#
# SPDX-License-Identifier: Zlib

"""The tag checks of the release workflow (.github/workflows/release.yml).

They are kept here so that they run the same way on a developer's
machine.  Run from the top of the tree, checked out at the tag (VERSION
is read from it).

  release-tag.py classify TAG
      An rc tag, zedbsd-<VERSION>-rc<N>, is built: prints kind=build,
      rc=N, version=VERSION.  A final tag, zedbsd-<VERSION>, promotes an
      rc: prints kind=promote, rc=, version=VERSION.  Anything else, or a
      tag whose version is not VERSION's, fails.
  release-tag.py rc FINAL_TAG [N]
      Prints the rc tag a final tag promotes: zedbsd-<VERSION>-rc<N>, or
      without N the highest-numbered rc tag of the version whose commit is
      in the final tag's history.  Fails when there is none.
  release-tag.py promotable RC_TAG FINAL_TAG
      Fails unless the rc's commit is in the final tag's history and the
      two differ only under docs/release/ (the notes may be finished after
      the rc; anything else must be built again).
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

VERSION_FORM = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(-[a-z0-9]+(\.[a-z0-9]+)*)?")
RC_NUMBER = re.compile(r"[1-9][0-9]*")


def fail(message: str) -> None:
    """Stops with a message."""
    print("release-tag: " + message, file=sys.stderr)
    raise SystemExit(1)


def git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", *args], stdout=subprocess.PIPE, text=True)


def version() -> str:
    """The version in the tree: one line of digits, dots, letters and dashes."""
    if not os.path.isfile("VERSION"):
        fail("no VERSION here")
    with open("VERSION") as stream:
        value = stream.read().rstrip("\n")
    if not VERSION_FORM.fullmatch(value):
        fail("VERSION is not a version: " + value)
    return value


def commit_of(tag: str) -> str:
    """The commit a tag names."""
    result = git("rev-parse", "-q", "--verify", "refs/tags/%s^{commit}" % tag)
    if result.returncode != 0:
        fail("no tag " + tag)
    return result.stdout.strip()


def is_ancestor(older: str, newer: str) -> bool:
    return git("merge-base", "--is-ancestor", older, newer).returncode == 0


def classify(tag: str) -> None:
    wanted = version()
    if tag == "zedbsd-" + wanted:
        print("kind=promote\nrc=\nversion=" + wanted)
        return
    prefix = "zedbsd-%s-rc" % wanted
    if tag.startswith(prefix):
        number = tag[len(prefix):]
        if not RC_NUMBER.fullmatch(number):
            fail("the rc number of %s is not a number" % tag)
        print("kind=build\nrc=%s\nversion=%s" % (number, wanted))
        return
    fail("%s is neither zedbsd-%s nor zedbsd-%s-rc<N> (VERSION is %s)"
         % (tag, wanted, wanted, wanted))


def rc(final: str, number: str | None) -> None:
    final_commit = commit_of(final)

    # The one named.
    if number:
        if not RC_NUMBER.fullmatch(number):
            fail("the rc number %s is not a number" % number)
        commit_of("%s-rc%s" % (final, number))
        print("%s-rc%s" % (final, number))
        return

    # The highest-numbered rc in the final tag's history.
    best = ""
    best_number = 0
    tags = git("tag", "--list", final + "-rc*").stdout.split()
    for tag in tags:
        text = tag[len(final + "-rc"):]
        if not RC_NUMBER.fullmatch(text):
            continue
        if int(text) <= best_number:
            continue
        if not is_ancestor(commit_of(tag), final_commit):
            continue
        best = tag
        best_number = int(text)
    if best == "":
        fail("no rc tag of %s in its history" % final)
    print(best)


def promotable(rc_tag: str, final_tag: str) -> None:
    rc_commit = commit_of(rc_tag)
    final_commit = commit_of(final_tag)
    if not is_ancestor(rc_commit, final_commit):
        fail("%s is not in the history of %s" % (rc_tag, final_tag))
    changed = git("diff", "--name-only", rc_commit, final_commit).stdout.split("\n")
    others = [name for name in changed
              if name != "" and not name.startswith("docs/release/")]
    if others:
        fail("%s changes more than docs/release/ since %s: %s"
             % (final_tag, rc_tag, " ".join(others)))
    print("release-tag: %s promotes %s" % (final_tag, rc_tag))


def main(args: list[str]) -> None:
    command = args[0] if args else ""
    if command == "classify":
        if len(args) != 2:
            fail("usage: classify TAG")
        classify(args[1])
    elif command == "rc":
        if len(args) not in (2, 3):
            fail("usage: rc FINAL_TAG [N]")
        rc(args[1], args[2] if len(args) == 3 else None)
    elif command == "promotable":
        if len(args) != 3:
            fail("usage: promotable RC_TAG FINAL_TAG")
        promotable(args[1], args[2])
    else:
        fail("usage: classify TAG | rc FINAL_TAG [N] | promotable RC_TAG FINAL_TAG")


if __name__ == "__main__":
    main(sys.argv[1:])
