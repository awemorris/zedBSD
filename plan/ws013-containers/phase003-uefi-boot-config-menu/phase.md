# WS013 Phase 003: UEFI `zedbsd.cfg` parsing and parameter assembly

Last updated: 2026-08-30

Phase ID: `ws013-p003`

Status: completed in `q031`; depends on completed `ws013-p002`

Parent: [WS013](../ws.md)

Tests: [WS013 review and test index](../tests/README.md)

## Objective

Read the selected FAT's required, bounded `/zedbsd.cfg`, load the kernel named
by its one loader-only `kernel=` directive from that same FAT, and assemble
all other lines into the existing textual kernel-parameter record. Boot
immediately: this Phase has no sections, menu, timeout, default choice, or
keyboard input.

## Initial grammar

The compact overlay form is:

```ini
kernel=vmunix
overlay-root=rootfs.img
overlay-data=data.img
swap0=swapfile
```

A native-UFS form is:

```ini
kernel=/vmunix
rootpart=PARTUUID=01234567-02
swap0=swapfile
```

The file uses printable ASCII with LF or CRLF line endings. A final line may
omit its newline. Empty lines are ignored. Every non-empty line is exactly one
`name=value` token with a non-empty name and value; spaces, tabs, quoting,
escaping, inline comments, continuation lines, UTF-8/BOM text, and section
headers are not part of v1. The first `=` separates the name, so identity
values such as `boot0=UUID=6740-911D` remain valid.

`kernel=` is the only loader-only name and must occur exactly once. Every
other syntactically valid line is retained as a kernel parameter in file
order. The loader does not whitelist the kernel vocabulary or duplicate the
kernel's semantic validation: known duplicates, unknown future names,
root-mode exclusivity, selector validity, and swap conflicts remain the
common kernel parser's responsibility.

The implementation fixes these initial resource bounds:

- at most 4096 configuration bytes;
- at most 64 physical lines, including empty lines;
- at most 511 bytes per physical line, excluding LF/CRLF;
- a kernel path shorter than 256 bytes after optional leading-slash
  normalization; and
- at most the existing 3071 bytes in the final parameter text, excluding NUL.

An over-limit file, line, line count, path, transformed token, or final record
is a visible configuration error. Parsing uses bounded storage and never
truncates to obtain a bootable result.

## Kernel directive

- `kernel=vmunix` and `kernel=/vmunix` both name `vmunix` at the selected
  FAT root. Subdirectories on that same volume are allowed.
- The normalized path must contain no empty, `.`, or `..` component,
  backslash, control character, device prefix, or volume selector.
- The loader converts the validated rooted path to the UEFI filename form,
  opens it through p002's selected SimpleFS root, and applies the existing
  bounded ELF64 validation and segment-loading rules.
- A missing, duplicate, empty, invalid, directory-valued, unreadable,
  oversized, or invalid-ELF kernel is a visible fatal error. No fixed
  `/vmunix` or `/VMUNIX.X64` fallback is attempted.
- `kernel=` is removed from the assembled parameter record and is never
  exposed as an unknown kernel parameter.

## Parameter assembly

Assembly preserves configuration order, except that a synthesized `boot0=`
is prepended when required. No embedded parameter string is merged.

### Selected FAT as `boot0`

If there is no `boot0=` line, prepend:

```text
boot0=UUID=<p002-selected-FAT-volume-serial>
```

For example, the compact overlay file on FAT UUID `6740-911D` becomes:

```text
boot0=UUID=6740-911D overlay-root=boot0:rootfs.img \
overlay-data=boot0:data.img swap0=boot0:swapfile
```

If at least one explicit `boot0=` line is present, preserve it and synthesize
nothing; an explicit duplicate reaches the common parser and fails there. The
loader never adds a hidden second value or silently changes an explicit
selector.

### Relative boot-file shorthand

- For `overlay-root=` and `overlay-data=`, leave an existing
  `boot0:`--`boot3:` reference unchanged. Otherwise interpret the value as
  a selected-FAT path and prepend `boot0:`.
- Apply the same rule to `swap0=`--`swap3=`, except that the unambiguous
  raw selectors `/dev/NAME`, `UUID=`, `LABEL=`, `PARTUUID=`, and
  `PARTLABEL=` remain unchanged.
- An unqualified `swapN=sda1` is ambiguous and is therefore treated as a
  selected-FAT file. A raw by-name device must be written
  `swapN=/dev/sda1`.
- Preserve explicit `boot1:`--`boot3:` references and their corresponding
  `boot1=`--`boot3=` definitions. Do not rewrite `rootpart=`, `init=`,
  or any unknown parameter.
- Validate every generated `boot0:PATH` using the common boot-file path
  restrictions before constructing the final string.

`rootpart=` therefore supports native UFS without a loader translation. A
native configuration may still use a selected-FAT swap file through the same
shorthand.

## Precedence and failure behavior

- UEFI `LoadOptions` is ignored unconditionally. It cannot replace, prepend,
  append, or repair `zedbsd.cfg`.
- A missing `zedbsd.cfg` has already produced p002's not-found error. If the
  selected file disappears or becomes unreadable before parsing, stop with a
  visible error.
- Invalid configuration, parameter overflow, or kernel load failure stops on
  the selected first candidate. Do not try a later configuration-bearing FAT.
- There is no embedded root/data/swap default and no legacy UEFI fallback.
- Names such as `timeout=` or `default=` have no loader behavior; as
  syntactically valid non-loader names they are merely passed to the kernel.
  A section header is malformed and fails parsing.
- The loader performs no console-input polling and makes no filesystem or
  firmware-variable writes.

## Implementation boundary

Keep discovery/order/device-path logic in p002-testable helpers and parsing,
path normalization, shorthand expansion, and final-record construction in
separately host-testable helpers. UEFI integration owns file I/O, diagnostics,
ELF loading, and complete cleanup before `ExitBootServices()`.

Update generated UEFI test media to include an explicit `zedbsd.cfg`; do not
retain a success fixture that boots only because compiled-in parameters or a
fixed kernel name remain available. Legacy non-UEFI image behavior is outside
this Phase and must not regress.

## Completion conditions

- Host fixtures cover LF/CRLF/final-line handling, empty lines, every grammar
  and resource boundary, malformed bytes, duplicate/missing `kernel=`,
  kernel path normalization, and exact final parameter strings.
- Exact-output fixtures cover synthesized and explicit `boot0=`, bare and
  explicit overlay/data paths, all four `swapN` slots, every unambiguous raw
  swap selector, explicit other boot slots, and final-string overflow after
  expansion.
- Native-UFS `rootpart=` passes unchanged and an OVMF cell reaches a native
  root; overlay configuration reaches the named lower/upper images and
  selected-FAT swap file.
- OVMF loads both a root `kernel=vmunix` and a safe subdirectory kernel from
  the selected p002 FAT, not automatically from the loader filesystem.
- Missing configuration, invalid configuration, missing/invalid kernel, and
  oversized input each print a visible error and stop without fallback.
- A deliberately valid UEFI `LoadOptions` parameter string is ignored, and
  the exact `zedbsd.cfg` result reaches the kernel.
- Boot shows no menu or countdown and consumes no keyboard input. Old
  section-based configuration fails rather than selecting a section.
- All files, roots, pools, pages, and memory-map resources have one documented
  owner and complete failure unwind before `ExitBootServices()`.
- GOP mode information is length-checked before dereference, and checked
  framebuffer geometry proves the visible byte count, the firmware-reported
  mapping span, address addition, 2 MiB rounding, and the 112-entry mapping
  ceiling before page-table construction.

## Actual results (2026-08-30)

- `run-zedbsd-config-host-test.sh` passed ordinary, Address/UndefinedBehavior
  sanitizer, and GCC analyzer variants. The corpus covers the independent
  file/line/count/path/final-record bounds, exact `kernel=` rules, synthesized
  and explicit `boot0`, overlay/data/swap shorthand, raw swap selectors,
  `rootpart=`, unknown parameters, and exact output text.
- `run-uefi-zedbsd-config-ovmf.sh` passed 7/7 q35/OVMF cells in
  `build/q031-ws013-ovmf-final`. The separate same-disk FAT normalized
  `kernel=/kernels/vmunix` to `A64 KERNEL kernels/vmunix`, loaded that file
  only from the selected second FAT, and delivered the exact synthesized
  `boot0=UUID=3333-4444` parameter record to the kernel.
- Invalid selected configuration and configured `kernel=missing.elf` each
  stopped on their visible loader error. Both retained a two-second bounded
  post-error observation interval and showed no later ELF or kernel parameter
  fallback; the missing-kernel medium deliberately contained root `VMUNIX` as
  a fixed-name fallback decoy.
- The LoadOptions cell injected the valid UTF-16 string
  `boot0=UUID=DEAD-BEEF init=/bin/false`. The loader logged that LoadOptions
  was ignored and the kernel instead received exactly
  `boot0=UUID=CAFE-BABE overlay-root=boot0:config-wins.img init=/bin/sh` from
  `zedbsd.cfg`.
- Existing BR-T46 production-loader cells provide the full-root complement to
  the focused discovery/parser harness. `amd64-uefi default` reached `login:`
  with FAT UUID `6740-911D`, the named lower/upper images, and 16383 swap
  slots; `amd64-uefi native` published GPT partition 3, resolved
  `rootpart=/dev/sda3`, and reached `init: system running`. The corrected
  disposable image helper updates both GPT entry arrays and both table/header
  CRCs; primary-GPT and forced-backup-GPT host scans each reported the UFS
  entry at LBA 264192 for 32768 sectors.
- The `amd64-uefi cross-boot` and `partuuid-reorder` cells also reached
  `login:` with exact UUID/PARTUUID bindings after the GPT fixture correction,
  while `amd64-bios native` remained passing. For a non-GPT MBR image, the
  corrected helper's output was byte-for-byte identical to the pre-change
  helper for the same appended UFS payload.
- Independent review identified short GOP descriptors and framebuffer-span
  arithmetic as missing boundary checks. `framebuffer.c` now calculates the
  bounded mapping plan before page-table construction;
  `run-uefi-framebuffer-test.sh` passes ordinary, ASan/UBSan, and analyzer
  cases for exact 112-page (224 MiB) acceptance, over-limit spans,
  address/rounding
  overflow, malformed stride, and undersized storage. A forced strict
  `BOOTX64.EFI` rebuild, the post-review OVMF 7/7 run, and fresh BR-T46
  amd64-UEFI default-overlay and native-root cells all pass.

## Reconsideration boundary

Return to planning if safe same-volume kernel paths require a second manifest,
if shorthand cannot be made unambiguous within the rules above, if the final
record cannot stay within the existing transport ABI, or if configuration
processing would require filesystem mutation.
