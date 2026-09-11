# ws019-p026 results — q159

Completed 2026-09-09. Noct copies image contents in bounded 64-KiB chunks with
64-bit byte progress, partial I/O handling and checked cleanup. Native directory
copying and file-count progress remain p028/p007; this result does not cover them.

Host evidence: installer-copy.noct passes 73 injected-operation cases in JIT and
interpreter modes. Real File API copying compared equal in both modes and
/dev/full produced failure. Destination and transaction regression tests passed;
the latter covers 102 scenarios. The amd64 disk-image build passed in
/tmp/zedbsd-q159-noct-copy-amd64.log and packaged module contents were checked.

Native evidence: temp/q159-public6/guest.log records loader 20992 bytes, kernel
1492584 bytes and rootfs 33554432 bytes copied, all six managed files published,
and `Installation complete.` with status 0. Cancellation and complete-destination
rerun also passed. The terminal result.json retains all three successful cases.
GPT/FAT boot/sentinel, payload marker, UEFI variables and production hashes match
their before values. Session 9572 terminated with exit 1 solely because the next
test invoked the shell's builtin cp with unsupported options. The guest log is
authoritative: the Python traceback shows the subsequently edited source line.
Do not relabel that overall result as PASS.

Production SHA-256: 4425d3655705ae08c6b3296d5cab88259dfebf8337635fe397acd576bdc93d0d

Rootfs SHA-256: 9d3d1d0560d7a3475293a4c5f7ac6e17a845b4e11023584522eee0e56935c37b

p004 conflict acceptance continues separately via run-install-conflict-qemu.py
on a copy of the installed target. p004 and p005 are not completed by this result.
