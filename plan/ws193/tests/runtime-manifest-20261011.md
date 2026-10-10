# WS193 p010: shared rootfs manifest / 2026-10-11

## Cause and source scope

RPi4 photo: `ld.so: cannot open dependency` precedes all three ssh-keygen failures. The interpreter exists; ssh-keygen/sshd both need `libutil.so`, `libcrypto.so`, `libc.so`. The current main UFS lacked `/lib/libutil.so` even though the library was built for linking OpenSSH.

`platform/common/userland-rootfs.mk` now defines installed commands, accounts, base data and dynamic runtime once. All six platform root-tree rules consume it. CPU compilation/CRT/link flags remain platform-owned. X68k declares its existing static-only ABI explicitly. The same portable libutil link rule now serves all five dynamic platform configurations. GNU cross compilers need `KERN_UAPI_NATIVE` (include/uapi/hosted.h); it is added to SPARC user/dynamic and X68k user compile flags. X68k's existing basic static linker macro also builds mount/sysctl, with umount as the established mount copy.

The content-stable root selection stamp includes the resolved common platform files. New runtime files therefore invalidate older staged roots even when the library itself was built before the previous root stamp. No C sources, HAL API, toolchain sources/build rules or shared build files are modified.

SPARC/X68k still have existing minimal bootstrap disk-container encoders separate from their root-tree targets. This refactor changes the rootfs manifest, not those bootstrap disk formats or their bootloader/kernel mount contracts; it does not establish complete selected desktop/package boot on those ports. Their native bootstrap image paths are a remaining architecture integration limitation, not proof of universal image/runtime equivalence.

## Verification

- Saved current config: [runtime-config](runtime-config-20261011.mk). Private worktree `.claude/worktrees/rpi4-sshd`, branch codex/rpi4-sshd, base main 3ba73d2c8.
- Shared LLVM read-only symlink and copies of current sysroots. A first ordinary named build tried to refresh the sysroot/LLVM dependency and failed at sandbox DNS; no toolchain build/install completed. Actual successful named builds explicitly pin the copied sysroot complete stamp with `make -o "$PWD/build/<arch>/sysroot/.zedbsd-sysroot-complete"`.
- `make -j16 -o "$PWD/build/arm64/sysroot/.zedbsd-sysroot-complete" build/arm64/dynamic/libutil.so`: PASS, warning/error 0; libc and libutil compiled in private build. Corresponding amd64 named build with CPU overrides: PASS, warning/error 0. Existing ELF validator: AArch64 shared-library, SONAME libutil.so, needed libc.so PASS; amd64 validation PASS in link recipe.
- `make -s` manifest inspection for amd64/rpi4/i386/pc98/sun4u/x68k, same selected `sh sysctl mount ls service llvm-runtime-license`: five dynamic normalized file maps identical; X68k differs only by dynamic runtime absence. Every installed built file is a manifest prerequisite. Compiler-specific automatic license selection remains the established registry policy; the fixture explicitly selects the same license so it compares the same resolved selection.
- Full current selected manifest comparison with read-only main: amd64 exact destination/source/mode map preserved; RPi4 differs only by added `/lib/libutil.so`. Existing rpthtest.so destination spelling retained.
- Production `build/arm64/rootfs/.stamp` recipe used for bounded host file-layout verification. Current selected main artifacts copied individually to private build; rebuilt libc/libutil used; Noct resources read-only from current source. Development-file install disabled for this host fixture; already-built direct inputs pinned with make `-o`, and production `.rootfs-config` evaluated normally. No new boot image was constructed from these fixtures. Full selected runtime staging PASS.
- Actual staged ssh-keygen and sshd DT_NEEDED followed recursively with llvm-readelf: all libraries resolve under /lib or /usr/lib; closure is recorded below. This is file-layout/ELF verification, not guest execution.
- Private root config stamp restored to its former format, timestamp older than root; cached libutil left unchanged. Production make re-stages once; a second unchanged invocation preserves root stamp mtime. PASS.
- All changed Make/source-manifest files reviewed against AGENTS/Guardrail and actual expanded rules: layer ownership, selection, modes, prerequisites, aliases, ABI linker flags, template escaping and incremental invalidation. `git diff --check`: PASS. No C formatter applicable; no C source changes.

## Additional build limits

Supplementary builds exposed pre-existing port/toolchain issues: PC/AT copied current i386 sysroot headers lack newer F_GETOWN_EX/f_owner_ex and sysconf definitions while compiling unchanged libc; SPARC unchanged wide-extra.c:175 has a type-limits warning promoted to error; X68k unchanged posix.c:8161/8318 tests defined weak default functions against NULL, rejected by GCC -Werror=address. Initial GNU tests also exposed missing native UAPI selection, fixed in the platform flags above. SPARC and i386 libutil source objects compiled, but complete library links on those ports are not claimed. The X68k added core link rules were reached but final link is blocked by existing libc compilation. These additional full-port builds are outside the scoped AArch64/runtime-layout acceptance; no warning suppression or unrelated libc rewrite performed.

No full SD image build, QEMU or physical ssh-keygen/sshd execution in this phase. Real RPi4 login/SSH and WS193-wide acceptance remain pending. Main integration requires exact commit approval; no push. Shared Master/Queue/Past Log/cache/GitHub projections remain Q1 pending.

## OpenSSH dependency closure

```json
{
  "usr/sbin/sshd": [
    "libutil.so",
    "libcrypto.so",
    "libc.so"
  ],
  "lib/libc.so": [],
  "usr/lib/libcrypto.so": [
    "libc.so"
  ],
  "lib/libutil.so": [
    "libc.so"
  ],
  "usr/bin/ssh-keygen": [
    "libutil.so",
    "libcrypto.so",
    "libc.so"
  ]
}
```

## Main integration follow-up / 2026-10-11

User explicitly approved main integration of `1b3d6cd03` (answer「mainへ統合する」). Clean main at 3ba73d2c8 fast-forwarded to the exact verified source commit; read-back confirms all 16 integrated files match the tested worktree. Source integration complete; preceding approval-pending text is historical. Documentation follow-up records this integration within the same approved scope. RPi4 physical host-key/SSH execution and whole-WS acceptance remain pending. No push; shared Master/Queue/history/cache/GitHub reconciliation stays Q1 pending.
