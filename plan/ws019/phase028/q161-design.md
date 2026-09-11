# q161 native tree utility design

120 active minutes; standing autonomous authorization. p004/p005 are accepted.

1. Fix find's implicit print once, using parsed action nodes rather than argv
   token scanning. Add -print0 and -fprint0 with checked output/close, checked
   readdir EOF and existing expression/traversal behavior. Owned output streams
   close on parse failure and normal exit. Test byte-exact filenames and fault
   injection, not just success on ordinary names.
2. Audit libc/kernel metadata APIs before cp -a/-R/-p implementation. Preserve
   existing exclusive and attribute-only options used by coexistence staging.
   Keep one copy context with a device/inode hard-link map across the whole
   tree. Preserve symlinks without following them; finalize directory metadata
   after descendants. Check all content/metadata/close failures.
3. The census is a NUL-separated regular-file manifest, not raw PTY output.
   Noct Process.spawn/read uses a PTY which can transform newline bytes. Define
   copy completion in a separate owned report file so diagnostics or unusual
   filenames cannot impersonate success records. Noct can poll while the child
   runs, assemble partial records, and display successful/total. Directories
   have separate preparation/finalization; no per-path cp processes that lose
   hard-link groups. Final report plus exit status and tree verification are
   required; progress alone is not acceptance.
4. Add focused host utility/fault tests, preserve coexistence staging cases,
   build maintained targets, and test enumeration/copy/attributes/progress in
   QEMU. Any syscall gaps become explicit bounded residual implementation; do
   not silently omit ownership, timestamps or links to force an acceptance.

This queue implements reusable utility capabilities. Full source-screen and
destructive installer integration remain p027 and p006/p007. If the timebox
ends with missing contracts, record them precisely and resume with a finite
successor; p028 is not complete from find alone.
