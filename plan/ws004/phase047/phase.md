# WS004 p047: AX211 / RTL8822BU lifecycle review corrections

Date: 2026-09-06
Combined ID: `ws004-p047`
Status: completed (`q085`), with P048 lifecycle acceptance.

Current result: the accepted review-3 design is implemented, and common,
RTL8822BU and AX211 PCI/boot/runtime production fixtures pass normal, sanitizer
and analyzer gates. All 30 P012 stories and three builds pass. See
[P048 evidence and hardware coverage limits](../phase048/results.md).
The pre-acceptance checkpoints below describe earlier stages and are superseded.

The user supplied `~/claude/zedBSD/plan/ax211-report-1.md` and
`~/claude/zedBSD/plan/rtl8822bu-report-1.md` and explicitly authorized evaluating
and incorporating their functional/stability corrections. This extends Q085
beyond the earlier userland-only P012 scope without reopening unrelated drivers.

Review close/open, scan cancellation, checked hardware-stop proofs, retained
resource ownership, operation joins and repetitive diagnostics. Correct verified
faults in the two adapter drivers and their existing common WLAN contracts.
Do not discard generation checks, release unquiesced DMA, or reset under active
leases merely because a review calls the condition benign. Record every finding's
disposition and the evidence against rejected suggestions.

Sequence: inspect each report against current sources; implement coherent
retirement/reopen handling; review caller/lock/ownership contracts; run serialized
`make -j16` with the amd64, PCAT and PC98 CI configurations; record remaining
acceptance cases. The user initially required a stop before acceptance, then
released it when accepting P048. The later lifecycle fixtures supply deterministic
runtime evidence; compilation and host fixtures do not establish RF stability.

Preserve firmware pins, public command grammar, USB endpoint/RF configuration,
and the direct nonblocking list contract. No commits, aggregate make check,
private credential reuse or `.internal` reads.

## Historical implementation checkpoint before the acceptance release

Both reports were evaluated and source corrections are implemented. See
[per-finding decisions and limits](review-response.md). The final sources pass
serialized builds (all exit 0):

- `make -j16 ZEDBSD_CONFIG=config/ci/config-amd64.mk`
- `make -j16 ZEDBSD_CONFIG=config/ci/config-pcat.mk`
- `make -j16 ZEDBSD_CONFIG=config/ci/config-pc98.mk`

Disposable logs are
`plan/ws005/temp/q085-wifi-scenarios/drivers-{amd64,pcat,pc98}-build.log`.
`git diff --check` passes. No tests or hardware operations were performed in
this implementation cycle. No commit was made. Stop at the pre-acceptance
checkpoint; keep P047 in progress for later lifecycle verification.

## Historical follow-up design review

The user requested evaluation of `~/claude/zedBSD/plan/review-3.md`.
See [review-3 assessment](review3-response.md). Static inspection confirms that
pending close needs an independent retry path and observable stop status; the
proposed unconditional concurrent quiesce is not safe with the current helpers.
The additional design is recorded, not implemented. Resolve these lifecycle and
P012 observation gaps before acceptance. This review changed documentation only;
the preceding build results apply to the unchanged source checkpoint.

The subsequent user instruction accepted that design, authorized P048 and
released the acceptance hold. P047's lifecycle verification now proceeds with
P048, followed by the complete P012 scenario campaign under Q085.
