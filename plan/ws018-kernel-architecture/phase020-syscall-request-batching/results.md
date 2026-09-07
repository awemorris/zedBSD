# q087 / ws018-p020 results

Date: 2026-09-06. Result: completed.
Baseline: 8d9f418 (Improve FS). No commit performed by this task.

## Result and design

The only production source changed since accepted q086 is `src/kern/syscall.c`.
Regular-file syscalls allocate min(request size, 256KiB), before acquiring the
file/VM I/O lease. Failed allocations halve the size, finally using the existing
512B stack buffer. This bounds simultaneous page-backed contiguous allocations
without forcing a 4KiB split on every request. Small requests allocate only their
required size. Allocation ownership and error cleanup stay in existing callers.

At the normal allocation size, 64KiB read/write/pread/pwrite now invokes
`file_io_transfer` once instead of 16 times. A single 64KiB readv/writev element
also uses one call. Multiple iovecs retain their separate boundaries; requests
above 256KiB and allocation-pressure fallback still split. This does not promise
one physical BIO: filesystem/cache/USB limits remain unchanged.

## Verification

- [Production syscall fixture](../tests/run-storage-syscall-stories.py): complete
  helper and all six syscall directions extracted verbatim, including production
  default constants; ASan/UBSan, warnings fatal. Four historical 512/4096 and
  IMOD 4000/0 cells plus the current 256KiB default pass. Current 64KiB read/write
  reports `backend_write=1 backend_read=1`. IMOD in this host fixture is only a
  label, not an IRQ measurement.
- Boundary sizes 0/1/511/512/513/4095/4096/4097/64KiB/cap/cap+1; a cap+1
  request splits twice under the production default. An injected 8KiB allocation
  ceiling produces eight transfers for 64KiB; complete allocation failure falls
  back to 128 stack-buffer transfers and subsequent I/O succeeds.
- All six directions exercise first/second backend errors, short transfer, EOF,
  first user-copy error and file-I/O begin failure, with lease/pin/allocation
  cleanup checks. Third-copy failure checks partial progress; positional I/O
  preserves descriptor position. Empty iovecs, growth-limit signaling, stream
  read termination and one-call PIPE_BUF vector writes pass. File/user-copy
  boundaries are test doubles; target coverage follows below.
- Serialized `make -j16 ZEDBSD_CONFIG=config/ci/config-{amd64,pcat,pc98}.mk`
  builds pass. The native fixture was rebuilt after adding a cap-crossing test.
- QEMU xHCI USB-root, disposable image, SMP4/512MiB: actual regular/positional/
  vector I/O, 256KiB+1 write/read through real kernel allocation, MAP_SHARED
  coherence, competing PIPE_BUF writes, overlay copy-up, production wifi-store
  save/failure/retry and grouped reboot persistence pass. Source image hashes
  remain unchanged. No physical USB or RF test was performed.

Evidence under [q087 logs](../temp/q087/):
[syscall](../temp/q087/syscall.log),
[build commands/results](../temp/q087/build-results.json),
[runtime commands/results](../temp/q087/runtime-results.json),
[native](../temp/q087/native.log), [benchmark](../temp/q087/bench.log),
[source hashes](../temp/q087/source.sha256),
[artifact hashes](../temp/q087/artifacts.sha256).

## Performance observation and limits

The same q086 benchmark warms a 64KiB file, then times four 64KiB positional
overwrites at offset zero plus one final fsync (262144 bytes written, not a
256KiB file). Prior 4KiB syscall batching took 70ms; this run took 20ms with
request-sized allocation. Both use QEMU xHCI USB root and IMOD=4000. Each is
one sample at 10ms guest clock resolution, not a throughput guarantee or a
physical-device measurement. The helper no longer imposes the observed 16-call
split; downstream batching/writeback remains in the follow-up matrix.

Q086's 50/50 result is retained as historical evidence; q087 reruns the affected
syscall and native integration coverage rather than claiming a new full 50 run.
