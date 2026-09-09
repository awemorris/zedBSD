# q159: public installer package and acceptance

Date: 2026-09-09
Timebox: 120 active minutes
Authorization: standing autonomous Priority instruction
Phase: ws019-p004

Prerequisite: q158 integrated cancel/install/rerun and protected hashes passed.
Apply reviewed private changes for zero-extra-capacity complete reruns. Register
an optional amd64-only zedinst package requiring base/noct, the executable /bin
launcher and /lib/zedinst modules. Enable it in the maintained amd64 CI profile;
preserve the user's config.mk. Verify host cases, package dependency expansion,
three maintained builds, image bytes/modes and a disposable standalone install.

Change native acceptance to execute the actual packaged launcher and ordinary
production rootfs. Run cancel/install/rerun plus a single-byte kernel conflict:
preserve original, corrupt only one byte, unmount, require refusal without
overwriting the conflict, restore and compare. Retain GPT/labels/sentinels/NVRAM
and source image hashes. Include source-alias and noninteractive refusal;
reuse relevant q148/q149 failure-boundary evidence without claiming it tested
the public launcher. Audit remaining p004 conditions before marking complete.
p005 installed NVMe-only boot remains a dependent phase, not implied by package
or installation success. Do not add a noninteractive installation mode.
