# Queue: native Wi-Fi credential-store acceptance

Last updated: 2026-09-01

QID: `q051`

Queue status: completed

Queue finished: **Yes**

Authorization: the user directed automatic continuation after the next
commit, origin synchronization, and push. q050 completed and pushed both VFS
prerequisites. The independent USB LAN implementation requested by the same
continuation is already complete in q049; q051 therefore selects the next
dependency-ready networking Phase rather than duplicating CDC ECM/NCM work.

Timebox: none. Complete the one selected Phase or record an exact `uncleared`
boundary. Stop only for a new product decision; ordinary implementation and
test defects remain within this Queue.

Parent: [master plan](master.md)

Previous Queue: [q050](queue-q050.md)

## Purpose

Close the consumer boundary for the existing root/per-user Wi-Fi credential
store. Exercise the real installed `/sbin/net wifi set-key` command inside
zedBSD, prove creator-effective ownership and durable atomic replacement, stop
the guest without an unmount, and verify the same writable media after reboot.

## Execution registry

| Priority | WS / Phase | Authoritative document | Status | Required result |
| --- | --- | --- | --- | --- |
| 1 | `ws005-p005` | [Phase](ws005-networking/phase005-wifi-credential-store/phase.md) | completed | Root, sudo-like, and ordinary-user stores pass ownership, atomicity, redaction, abrupt-stop, and second-boot persistence acceptance through the real `/sbin/net` |

## Dependencies and exclusions

- `ws001-p022` and `ws001-p023` completed in q050 and are regression inputs,
  not Queue items.
- The p002 v1 credential/profile contract is frozen; no new format, path,
  locking, or privilege decision is needed.
- This Queue does not implement WLAN ioctls, `networkd` WLAN framing,
  association, WPA2 radio behavior, DHCP, or physical Archer acceptance.
- No live host `/etc/wifi.conf` or user profile may be read or changed. All
  media and accounts are test-only and disposable.

## Fixed boundaries

- Invoke the real guest `/sbin/net wifi set-key`, without a shell wrapper
  interpreting credentials. Test root, real-user/effective-root sudo-like,
  effective ordinary-user, and ordinary-user update paths.
- Root always selects `/etc/wifi.conf`. A non-root effective UID selects the
  passwd-record home and ignores `HOME`. The two stores and persistent lock
  files must be regular single-link files with mode `0600`; owners are `0:0`
  and the test account's UID/GID, respectively.
- A successful update uses a persistent lock, a validated same-directory
  temporary, file `fsync`, atomic rename, and containing-directory `fsync`.
  The lock inode remains stable, the target inode changes on replacement, and
  no temporary name remains.
- `/run/networkd.sock` is not required for local `set-key`, and
  `/etc/net.conf` remains byte-for-byte unchanged.
- Test passphrases are synthetic secrets. They must not appear in guest output,
  QMP/serial/debug logs, controller diagnostics, command records, or retained
  evidence. Guest failure output names only a stage and errno.
- Stage 1 writes and durably records its PASS marker, then the controller sends
  QMP `quit` immediately. It does not request guest unmount or shutdown. Stage
  2 reuses the same writable overlay media and verifies the final profiles,
  metadata, locks, absence of temporary/fake-HOME files, and unchanged
  `net.conf`.
- Use a private bounded output directory and source-image hash checks. Do not
  use `.internal/`, aggregate `make check`, or add a Python script; the runner
  is Noct and the supported build gate is `make -j16`.

## Required evidence

1. Existing ordinary, ASan/UBSan, and fixture-scoped analyzer credential-store
   gates pass, together with retained WS011 console/boot and WS005 recovery/
   networkd-auth regressions.
2. One private amd64/UEFI overlay image boots twice. Stage 1 executes every
   real-command identity transition and is stopped immediately after PASS;
   stage 2 verifies the same writable media without relying on clean shutdown.
3. Every profile generation parses as one complete `wifi-conf 1` document;
   exact metadata, stable lock inode, replaced target inode, and no residue are
   verified before and after reboot.
4. Secret scans cover all retained text evidence. Frozen source images remain
   unchanged; private writable credential media is not retained after success.
5. `make -j16` and `git diff --check` pass.

## Completion definition

q051 completes when `ws005-p005` meets every Phase condition with the real
guest command and abrupt-stop/reboot evidence, or is honestly `uncleared` with
one reproducible product blocker and a concrete resume condition. Successful
completion releases p005 to `ws005-p006`; it does not claim WLAN hardware or
radio operation.

## Execution result

The Phase completed without a new product decision.

- The store reader now retains the opened target's identity and, while still
  holding the shared persistent lock, revalidates the named inode after the
  complete parse. A deterministic non-cooperating rename cell proves that a
  replaced target fails closed with `EBUSY`, publishes no replacement model,
  preserves the caller's previous model, and exposes no credential.
- The credential-store ordinary, ASan/UBSan, compiler-analyzer, and parser/model
  gates pass. Retained WS011 console/boot and WS005 recovery/networkd-auth
  regressions also pass.
- One private amd64/UEFI q35+xHCI overlay image passes both launches. Stage 1
  runs the real `/sbin/net wifi set-key` as root, sudo-like effective root,
  effective ordinary user, and ordinary user, then receives QMP `quit`
  immediately after its PASS marker. Stage 2 verifies the same writable media.
- Both stores and locks have exact owner/mode/link metadata, lock inodes remain
  stable across updates, target inodes change, no transaction temporary or
  malicious-`HOME` output remains, `/run/networkd.sock` is unnecessary, and
  `/etc/net.conf` is byte-for-byte unchanged.
- The frozen source image is unchanged, synthetic credentials appear in no
  retained output stream, and the private writable credential image is deleted
  after success. `make -j16` and `git diff --check` pass.

Final disposable evidence: `/tmp/ws005-q051-final-001`.
