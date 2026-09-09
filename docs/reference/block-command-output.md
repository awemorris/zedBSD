# Structured block and file command output

Status: current; capacity output reconciled in q151

`diskpart --machine list` includes whole devices and partitions. Each tab-separated
record has this field order:

```text
device NAME REGISTRATION PARENT FLAGS SECTOR_SIZE SECTOR_COUNT PARENT_OFFSET
```

The actual separators are tabs. Registration and parent are live kernel device
numbers; they are not persistent disk identities. Parent offsets and counts are
logical sectors. Flags retain the BLKGETINFO values (read-only=1, removable=2,
partition=4). Human `diskpart list` continues to show whole disks only.

`diskpart --machine show DISK` reads the on-disk table and emits:

```text
device ...
table FORMAT RESTRICTIONS ACTIVE_COUNT
range BYTE_OFFSET BYTE_LENGTH
partition SLOT START COUNT TYPE UUID ATTRIBUTES
```

FORMAT is `gpt` or `mbr`. Restrictions are the existing parser's degraded and
unsupported bits. Range records describe the complete logical sector containing
the MBR and, for each available GPT copy, its header sector and padded entry
array. They allow callers to hash the actual metadata including names and GUIDs.
Partition labels are available in human output; machine partition records omit
label strings and retain fixed numeric/GUID fields. Machine output is read-only;
combining `--machine` with editing or reload is rejected. The on-disk table and
live registrations must still be compared by callers, as for human output.

`blkid -o export [-s TAG ...] [--] DEVICE ...` selects TYPE, UUID, PARTUUID,
LABEL or PARTLABEL. Without `-s`, all available fields are shown. Each device
starts with DEVNAME and ends with a blank line. Values escape whitespace,
control/non-ASCII bytes, backslash, equals and double quote as `\xhh`. `-o full`
retains the human output; `-s` also applies there. Export returns 2 for no requested
identity or an unsupported identity query, 1 for I/O failure, and 0 for success.
Multiple device results are combined by bitwise OR (so mixed failures can return 3). Invocation errors return 2. The historical full output retains its empty-success
behavior for unsupported identity queries. `TYPE=vfat` does not distinguish
FAT16 from FAT32; use the validated BPB when that distinction matters.

`stat -c FORMAT [--] FILE ...` supports `%d` device number, `%i` inode,
`%f` hexadecimal complete mode, `%s` decimal size, `%a` octal permissions,
`%u` UID, `%g` GID, `%n` filename and `%%`. It prints one newline per operand.
Unknown fields and a trailing percent sign are rejected before any record is
printed. Like the original command it uses lstat: a symlink is reported as a
symlink. For example, the installer reads `%d:%i:%f:%s:%a:%u:%g` and separately
retains the operand, avoiding pathname delimiters in the identity record.

These commands propagate record-output failures. Their output is an observation,
not a retained file descriptor or transaction-wide claim. Installer source
provenance comes from the retained boot selectors described in
[boot provenance](boot-provenance.md).

## Capacity observations

`df [-kP] [--] [FILE ...]` prints a header and one row per requested path.
Without operands it observes `/`; it does not enumerate every mounted filesystem.
`-k` selects 1024-byte units instead of 512; `-P` selects the existing portable
single-line layout and can be combined with `-k`. For example:

```sh
df -Pk -- /run/zedinst/payload
```

Columns are the operand, total units, used units, available units, rounded-up
capacity percentage and the operand again. Thus the first/last columns are
paths supplied to df, not independently resolved device or mount identities.
For machine consumption use controlled paths without whitespace and require
the exact expected row count, header, numeric fields and successful exit.

The implementation reports complete units, checks conversion overflow and
rejects zero fragment size or inconsistent free counters. Capacity is
`ceil(100 * used / (used + available))`, with zero for the empty denominator.
Errors on one operand do not suppress subsequent operands, but make the
overall exit unsuccessful. A final buffered output failure also fails the
command. Invocation errors return 2; observation/output errors return 1.

Successful `df` does not reserve space or guarantee that a later allocation
succeeds. An installer must separately account for cluster rounding, directory
growth, concurrent changes and staging errors. [df source](../../userland/base/df/main.c)
and [q151 checks](../../plan/ws019-installation/phase020-df-capacity/results.md)
cover arithmetic and complete-error reporting.
