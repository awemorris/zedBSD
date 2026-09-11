# q162: source selection and live root-image admission

90 active minutes, standing autonomous authorization; p028 passed.
Dedicated provisioning remains p006/p007 outside this queue.

Actual code: kern/vfs.c mounts the lower image read-only through a loop and a
private mount before creating the root overlay, then releases transient setup
paths. Ordinary mount output cannot establish that relationship. sysctl already
exports physical boot origins; drv_loop_backing_disk_ref pins the backing disk
but alone does not prove the lower-root mount or backing-file identity.

1. Expose a coherent read-only root-image observation through existing sysctl.
   Resolve live root/overlay lower and loop backing under appropriate references:
   kind, mounted/readonly state, loop device and backing partition/file identity.
   Userland resolves the controlled rootfs.img name on the admitted source and
   matches that identity; the kernel does not reconstruct a private pathname.
   No raw pointers or configuration-only mounted-state assertions.
   Native boot explicitly has no mounted root image; query failures are errors.
2. Parse the bounded record in Noct and match it to the discovered physical
   installer source and rootfs.img identity. Recheck across interaction/shell
   return. A private lower mount qualifies; mere filenames, native boot and
   unrelated loop images do not.
3. Add the first text source screen: disk enabled, HTTP unavailable. Preserve
   the approved visual direction, cancellation and frontend callback separation.
   Missing image produces the requested not-booted-from-installer error before
   target writes. Integrate with coexistence; full mode/disk flow follows p006.
4. Fix the literal 4;2m artifact by checking Noct initialization and console CSI
   parsing, rather than hiding it with a repaint.
5. Test observation/identity errors and transitions on host; build maintained
   images; exercise private-root USB admission and native/no-image refusal in
   disposable QEMU. Refusal/cancel preserves target hashes. Capture actual UI.

Implement missing ownership prerequisites explicitly. On timebox expiry record
facts/resume conditions as uncleared; do not substitute boot text for live proof
or claim completion of the entire installer.
