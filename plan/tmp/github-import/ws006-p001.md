<!-- awesome-plan project=zedbsd record=ws006-p001 -->

既存計画の取り込み。本文はローカルの現行記録、リポジトリ資料へのリンクは基準コミットの履歴です。Issue作成・open状態は実行承認や未完了判定を意味しません。Awesome Planの状態同期導入前のため、実際の状態は本文を参照してください。

元資料: `plan/ws006/phase001/phase.md`

親: [ws006](https://github.com/awemorris/zedBSD/issues/7)

# WS006 Phase 001: evdev compatibility profile

Last updated: 2026-08-25

Phase ID: `ws006-p001`

Status: complete

Parent: [WS006](https://github.com/awemorris/zedBSD/issues/7)

Product reference: [evdev profile](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/docs/reference/evdev.md)

Tests: [WS006 test index](https://github.com/awemorris/zedBSD/blob/67b28ce04445787ad9225be4a703e3323587b6f1/plan/ws006-input/tests/README.md)

## Objective

Freeze an independently implemented, Linux/FreeBSD-oriented evdev subset before
creating `/dev/input/eventN` or migrating any producer or consumer.

## Fixed decisions

- The authoritative header is `<zedbsd/input.h>`; Linux and FreeBSD include
  paths are source-compatibility wrappers.
- `struct input_event` uses zedBSD's native `struct timeval`: 24 bytes on x86-64
  and 20 bytes on i386.
- Event timestamps are `CLOCK_MONOTONIC` producer-observation times.
- The first constant set covers PC keyboards, relative mice/wheels, absolute
  pointers, and basic multitouch positions, not the entire Linux catalog.
- zedBSD ioctl encoding is retained. Symbol/semantic compatibility does not
  imply binary-compatible ioctl numbers.
- `EVIOCGRAB` excludes other evdev readers but not the kernel console broker.
- Device numbering is dynamic; event injection is out of scope.

## Work packages

- [x] Audit current console and mouse event consumers.
- [x] Compare the current Linux and FreeBSD public event layouts/interfaces.
- [x] Publish the event structures, initial constants, and ioctl subset.
- [x] Add Linux and FreeBSD source include wrappers.
- [x] Specify timestamp, packet, overflow, read/poll, detach, grab, permission,
  discovery, and injection policies.
- [x] Publish an explicit compatibility-difference table.
- [x] Pass header/layout checks for x86-64 LP64 and i386 ILP32.

## Non-goals

No kernel input core, device node, console bridge, Xzed migration, legacy UAPI
removal, or USB HID implementation is part of this Phase. Declared operational
semantics are the acceptance contract for subsequent implementation, not a
claim that an event device already exists.

## Evidence

Both IN-T00 `-fsyntax-only` commands pass with `-Wall -Wextra -Werror`, including
the compatibility wrapper paths. Repository documentation link validation also
passes after publishing the profile.

## Resume point

Extract IN-01 as `ws006-p002` to implement the input core and event character
device against this frozen subset. Reopen p001 only if implementation exposes
an unrepresentable ABI requirement; additive constants do not require reopening
the core layout decision.
