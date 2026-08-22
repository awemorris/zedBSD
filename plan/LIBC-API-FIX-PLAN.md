# libc API review correction plan

## Result

All findings from the ordered 466-interface review have been corrected.  The
per-interface disposition is recorded in `libc-api-audit.csv`; there are no
remaining `pending` review or fix rows.

## Completed corrections

1. **Floating point:** build all required ISO C17 libm entry points from the
   pinned musl source; keep zedBSD error helpers outside the vendor submodule;
   maintain a per-thread software `fenv`; report domain and range exceptions.
2. **Nonlocal jumps and integers:** add compiler-assisted `setjmp`/`longjmp`,
   `<inttypes.h>` conversions, and their public declarations.
3. **stdio:** add the remaining formatted and unformatted entry points; support
   scansets, widths, wide arguments, cookie streams, purge, reassociation,
   dynamically sized `fgetln`, and BSD allocation helpers.
4. **stdlib and time:** connect LIFO exit handlers, add sorting/searching,
   allocation overflow helpers, multibyte wrappers, program naming,
   `timespec_get`, and an entropy-device/ChaCha `arc4random` implementation.
5. **wide and UTF:** add the remaining wide string/numeric/stream APIs and
   stateful UTF-16/UTF-32 conversions, including surrogate continuation rules.
6. **BSD strings and err:** add the selected extensions, remove out-of-bounds
   version comparison, make timing-safe comparison selection branchless, and
   implement the complete `err(3)` family.
7. **integration:** link the added common sources into static and shared libc on
   every maintained platform makefile and remove duplicated compatibility
   symbols.

## Verification gates

- `python3 tools/libc-api-audit.py --repo . --output plan/libc-api-audit.csv`
- `python3 tools/libc-api-review.py plan/libc-api-audit.csv`
- `make ARCH=amd64 libc-objects softfloat-objects`
- setjmp/longjmp host round-trip smoke test
- `make ARCH=amd64 build-boot-disk-image`
- AMD64 QEMU boot through the interactive root shell
- `make ARCH=pcat build-boot-disk-image`
- `make ARCH=pc98 build-boot-disk-image`
- ARM64 kernel link and image-contract check
