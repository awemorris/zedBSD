# q151 capacity-command result

Date: 2026-09-09
Status: completed

`df` now checks capacity conversion without constructing an overflowing byte
product, calculates upward-rounded percentages without `used * 100` overflow,
rejects inconsistent statvfs counters and zero fragment size, and propagates
buffered stdout failures. The standard -P option accepts the existing portable
row format, including combined -Pk and --. The default root operand no longer
overwrites argv's NULL terminator. Multiple operand errors remain observable
while later operands are attempted.

Evidence:

- `tests/run-df-capacity-host.sh`: 20,000 independently checked arithmetic
  cases using a host 128-bit oracle, plus zero/full/boundary conversion,
  inconsistent records, option/default/multiple operands and `/dev/full`.
  Both ordinary and ASan/UBSan runs pass in `/tmp/zedbsd-q151-df-host2.log`.
- Initial fixture compilation failed because its statvfs replacement also
  replaced the structure tag; a function-like fixture macro corrected that.
  No production change was needed for the fixture failure.
- Serial `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk`
  gates all exit 0, logs `/tmp/zedbsd-q151-{amd64,pcat,pc98}.log`.
- Focused whitespace checks pass. The command contract is in
  [block output](../../../docs/reference/block-command-output.md).

No new physical or QEMU runtime claim is made for this userland arithmetic
change. p004 still needs the full installer admission/integration boundary;
successful df output is an observation, not a space reservation.
