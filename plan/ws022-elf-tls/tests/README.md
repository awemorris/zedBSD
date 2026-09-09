# WS022 focused tests

- `build-tls-fixtures.sh OUTPUT`: project clang/ld.lld compiler corpus for amd64/i386. Inspect with project llvm-readelf/objdump. Four valid and ten malformed ELF images per architecture.
- `run-static-tls-host.sh OUTPUT`: compile the actual static runtime allocator; ordinary and ASan/UBSan ownership, clone, zero-fill, alignment and failure recovery.
- `qemu-tls-loader.sh OUTPUT`: private image and disposable runtime copy; 8 concurrent pthreads, 100 repeated create/join, signal/errno/fork, failed create recovery, malformed exec rollback, valid/empty/zero-only/offset TLS and dynamic plugin regression.

Run serially from the repository root:

```sh
bash plan/ws022-elf-tls/tests/build-tls-fixtures.sh plan/ws022-elf-tls/temp/q128-fixtures
bash plan/ws022-elf-tls/tests/run-static-tls-host.sh plan/ws022-elf-tls/temp/owner
ZEDBSD_TEST_CONFIG="$PWD/config/ci/config-amd64.mk" COMMAND_TIMEOUT_SECONDS=120 \
  bash plan/ws022-elf-tls/tests/qemu-tls-loader.sh plan/ws022-elf-tls/temp/amd64
TLS_TEST_ARCH=i386 ZEDBSD_TEST_CONFIG="$PWD/config/ci/config-pcat.mk" COMMAND_TIMEOUT_SECONDS=120 \
  bash plan/ws022-elf-tls/tests/qemu-tls-loader.sh plan/ws022-elf-tls/temp/i386
```

The QEMU runner currently consumes the fixed `temp/q128-fixtures` corpus. amd64 uses Q35/xHCI USB boot; i386 uses the supported PC/PIC/IDE topology. It checks actual guest success markers rather than trusting the shell's cross-command `$?` behavior. All builds use `make -j16`; no aggregate check or physical media access.
