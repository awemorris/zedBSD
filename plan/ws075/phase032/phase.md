<!-- awesome-plan project=zedbsd record=ws075-p032 -->
# ws075-p032: i915 の GT の object の表を必要に応じて伸ばす（BUG-120）

Status: blocked（UAT 待ち）（実装済み・host 試験 PASS。i915 は QEMU に無く、実機 5330 で窓 30 個以上の確認が要る。2026-10-09 P1、ユーザーの規則 2026-10-08 夜）
Disposition: normal
Parent: [WS075](../ws.md)
Bug: [BUG-120](../../bugs/BUG-120.md)
Queue: Q1 の dispatch（P1 generation11、2026-10-03。user「P1,P2ともに、もう対応可能なバグ修正はないんですか？」）

## 範囲と実装

窓を 30 個ほど開くと `gt memory: object pool exhausted`（表が固定の 128）。各 context の ring・context の image・page table が GT の object を使う。

- `src/drivers/gpu/i915/memory.h`・`memory.c`: 固定の配列 `objects[128]` を、128 個ずつの block（`kern_calloc`）の表に替えた。空きが無ければ block を
  足し（最大 16 block = 2,048 個、`I915_GT_MAX_OBJECTS`）、log `i915: gt memory: object pool grew to N slots`。block は device が動く間は動かさず
  解放しないので object の address は変わらない。fini は kept の object（display が読むかもしれない）を持たない block を解放する。
- GT の GGTT の窓（`I915_GT_GGTT_PAGES`）は別の上限で、変えていない。

## 検証

- build: kernel（`build/p1-q640/vmunix`）warning 0、`amd64 vmunix check: PASS`。
- host: `sh plan/ws075/tests/host-gt-pool.sh` → `host-gt-pool: PASS`（memory.c を ASan・UBSan で。1,000 個を一度に作れて block 8、伸びる間も address が
  変わらない、半分を消して作り直すと伸びない、2,049 個目は「exhausted」で断る、fini が kept の block 以外を解放）。
- 実機（5330 の i915、窓 30 個以上）は未実施（S2）。QEMU には i915 が無い。
