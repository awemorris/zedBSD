# tests/display

The host tests of the i915 display, built with the host compiler and never by the kernel build.

- `host-xd-*-test.c` test the display 11 to 13 code (`display/xd-*.c`) on the register model of
  `xd-fake-regs.c`; `host-output-link-test.c` tests the planned link of an output (`display/output.c`).  One
  script runs all of them, together with the check that no `xd-*.c` reaches `internal.h`.
- `host-gen9-*-test.c` test the display 9 code (`display/gen9-*.c`) on `gen9-fake-hw.c`.
- `host-bootfb-test.c` and `host-capture-test.c` test the boot framebuffer and the capture display.

Every test is built by the display host library: all of `display/*.c` (each its own
translation unit) plus `trace.c`, `host-kernel.c`, `host-test.c` and the test's own sources, under
ASan/UBSan, linked with `--gc-sections`.

- `host-kernel.c` stands in for the kernel services the tested paths reach (allocator, locks, log, clock,
  interrupt state) and, so that the production files link, for the services they must not reach (MMIO BAR,
  PCI, GT memory, work queues, worker): a call to one of those ends the test with its name.
- `host-test.c` holds the tally (`<test>: N checks, M failures`) and reads the reference files (the register
  and DPCD dumps of a Latitude 5330 under Linux) from the directory a test is given.
- A failed check prints `FAIL: <what>` (every message is unique).
