<!-- awesome-plan project=zedbsd record=ws347-p008 -->

# ws347-p008: blitter の無い GPU で表示の写しを shader で行う

Status: in-progress（2026-11-13 Q1、q2030-i01 P4。実装と host 試験・kernel の build は済み、判定は Q1）
Parent: [WS347](../ws.md)

## 目的

2026-11-13 ユーザーの決定: blitter（CAP_BLT）が無い時は、表示の写しも shader で行う。

今の `src/drivers/gpu/ghci/display.c` は、copy の present（ordinary storage を driver の 2 つの frame の片方へ写す）で blitter が無いと CPU が写す（`copy_by_processor`、discrete の GPU では PCIe の local memory の窓を通す）。VF2＋KC705 では PCIe が遅く、1080p の frame の CPU の写しは表示を大きく遅らせる見込み。Vulkan の転送は既に blitter が無ければ driver の shader の program（`vk-execute.c` の転送の program、p003）で行う。

## 範囲

- blitter が無い時の copy の present を、driver の shader の program（compute の dispatch）で session 0 から写す。frame は session 0 に map する（今は blitter の時だけ map）。既存の Vulkan の転送の program と道を共有できるかを先に確かめ、共有できる形を本来の形とする。
- CPU の写しは、shader の program が使えない時（compiler が program を拒む等）の代わりの道として残すかを決めて書く。
- unified の GPU（UMA）でも shader が良いか（CPU の直の写しとの比較）を本来の形で決めて書く。
- HAL の API・Makefile・mk/ は変えない。

## 受け入れ

- host 試験（WS290 の試験の道具、blitter の無い profile の `minimal`・`flat`・`kc705-min`）で、copy の present が dispatch になり blitter の packet も CPU の写しも出ない。blitter の有る profile は今どおり blitter。全 PASS。
- vf2・x86_64 の kernel が warning 0。実機の速さは p004。

## 範囲に足した事（2026-11-13 Q1 経由のユーザーの決定）

- `CONFIG_GHCI_POLL`・`CONFIG_GHCI_HOST_UNCACHED` を build の option にしない: 割り込みの有無と DMA の coherent は driver が bus から調べて決める基本の機能なので、source から option を外し（`#ifndef` の既定と `if (CONFIG_GHCI_POLL)`・`|| CONFIG_GHCI_HOST_UNCACHED` の分岐）、検出だけで決める。p003 の mk の 2 つの key の案は取り下げ。
- CLIP_POLY = 0（clipper の無い GPU）は何も言わずに欠けてよい（minimal は実用ではない）: attach の log の shortfall から CLIP_POLY を外し、software の clip は作らない。

## 確かめた事（2026-11-13 P4、設計の前）

- **driver の meta の program は kernel の中**: `src/drivers/gpu/ghci/vk/` は kernel の source（`mk/kernel.mk` の GHCI の節）で、`vk-meta.c` が IR を組み、kernel の中の GSI1 compiler（`compiler/gsi1-*.c`）で GPU ごとに 1 回 compile し（`ghci_vk->meta`）、VkDevice の code heap に置く。kernel の中で shader の program を出す道は既にあり、新しく要る物は無い。足りないのは「session 0 に置く」所だけ。
- **共有できる物**: copy の program（`GHCI_VK_META_COPY`、要素 1〜4 byte・行・slice の stride を push 定数で受ける）は表示の写し（4 byte の要素 × 幅、行の stride が src と frame で違う）をそのまま表せる。計画（`ghci_vk_meta_copy`）、compile の cache と拒否の印、pipeline の record の書き方、resource table の書き方、push 定数と dispatch の packet もそのまま使える。違うのは置き場所（VkDevice の session の code heap と temp の memory → session 0）と、submission（Vulkan の queue → driver 自身の仕事 `ghci_cp_submit(NULL)`）だけ。
- **core と executor の境**: `display.c` は driver の core で、executor（`vk/`）は weak に link される（core の host 試験は executor 無しで link する、shader unit の無い GPU は executor 無し）。core から executor の関数を直に呼ばず、既にある `struct ghci_executor_ops` に `copy_rows` を足して呼ぶ。HAL の API ではない（driver の中の表）。
- **仕様**: §6.0 は session 0 で compute を出してよく、PSO・descriptor・buffer を session 0 に map する事を求める。§11.5 は host が table を書き換えたら `INVAL_DESCRIPTOR`、code を書いたら `INVAL_ICACHE`。fence は前の書きが memory に届いてから書かれる（§11.5）ので、fence を待てば flip してよい。
- **phase.md の前提の直し**:
  - minimal・flat の profile は DISP（表示）を持たない（`plan/ws290/tests/ghci-model.c`）。copy の present の場面は作れない。blitter の無い表示の profile は **kc705-min**（discrete・flat）・**no-blt**（unified・walker）、と kc705-discrete から CAP_BLT を外した物（discrete・walker・local の frame）。試験はこの 3 つ。
  - kc705-min は walker が無いので driver は local memory を使わず（`memory.c` の `ghci_memory_local_start`）、frame も storage も host memory。今の CPU の写しは PCIe の窓を通らず host → host（frame は表示の controller が PCIe 越しに読む）。PCIe の窓を通るのは walker の有る discrete の GPU だけ。flat の discrete の address の振り分けを台帳の B32 に足した。

## 設計（2026-11-13 P4）

### 何が写すか（`display.c` の `copy_engine`）

| GPU | frame | 写す物 |
| --- | --- | --- |
| blitter 有り | local memory | blitter（今どおり） |
| blitter 有り | host memory | processor（今どおり） |
| blitter 無し、executor 有り（shader unit と GSI1） | local・host とも | driver の copy の program（session 0 の dispatch） |
| blitter 無し、executor 無し（shader unit 無し） | local・host とも | processor（local は窓を通す） |

- program が写せない時（compiler の拒否、pipeline の record に入らない、address が 4 GiB 以上で descriptor が届かない）は `copy_rows` が何も置かずに ENOTSUP を返し、その present は processor が写す。compiler の拒否は GPU ごとに 1 回で覚える（Vulkan の転送と同じ印）。

### 根拠

- **CPU の写しを代わりの道に残す**: shader unit の無い GPU（表示だけの GPU）には GPU で写す手段が無い。compiler の拒否や 4 GiB の上の memory でも表示を止めない方が良い。残すのは「GPU で写せない時」だけで、blitter の無い GPU の既定の道ではない。
- **unified でも shader にする**: (1) ユーザーの決定は「blitter が無い時は shader」で、frame の置き場所で分けていない。(2) processor の写しは 1 frame（1080p で 8 MB）の間 CPU を 1 つ占める。DMA の coherent でない bus では host の view が uncached になり（`struct ghci_allocation` の view）、CPU の uncached の読みは遅い。shader の写しは GPU が走らせ、thread は fence を待って眠る。(3) 写しは ring の中で順に置かれ、frame への書きは fence の前に memory に届く（§11.5）。processor の写しは frame の view の cache の扱い（窓の読み返し）を自分でする必要がある。
- **blitter 有り・host の frame は今どおり processor**: 受け入れ（blitter の有る profile は今どおり）に従った。blitter 無しで unified を shader にしたので、blitter 有りの unified も blitter に寄せるのが揃った形だが、範囲の外なので Q1 への案として残す（下の「残り」）。
- **専用の program を作らない**: 表示の写しは copy の program でそのまま表せる（要素 4 byte、`per_row` = 幅、`rows` = 高さ、stride は src と frame）。要素ごとに UDIV・UMOD が 1 つずつ（GSI1 の divu・remu、hardware）と、整列していない時の分岐（predicate で消える atomic）が余分。1 frame の速さは p004 で量り、遅ければ「4 byte に揃った行の写し」の速い道を同じ program に足すかを決める。

### 形

- `ghci.h`: `struct ghci_rows`（session 0 の 2 つの address、行の byte、行の数、各側の stride）と `ghci_executor_ops.copy_rows`。
- `vk/vk-meta.c`:
  - `ghci_vk_copy_rows`: copy の計画（`ghci_vk_meta_copy`）→ 初回だけ program を session 0 に置く（`rows_place`）→ resource table を書く → head の barrier（`GHCI_VK_HEAD_FLAGS`: ICACHE・TEX・L1・DESCRIPTOR）、bind、step（LOAD_CONST + DISPATCH、65535 workgroup ごと）、tail の barrier（`GHCI_VK_TAIL_FLAGS`）→ `ghci_cp_submit(device, NULL, ...)` で driver 自身の仕事として ring に置き、ticket を返す。lock（`rows.lock`）が置き場所・table・ring への置きを 1 つにする。
  - `rows_place`: host memory の 1 つの run に PSO・code・table（64 byte 揃え）を置き、session 0 に executable・GPU から読むだけで map。run は detach まで（`ghci_vk_rows_release`、`node_detach` から）。table を code と同じ run に置くのは、run を書くのが driver だけで GPU は読むだけだから（Vulkan の heap の「GPU が書く物・client が埋める物を実行可能な所に置かない」に反しない）。
  - Vulkan の道と共有に分けた関数: `meta_programs`（compile の cache と拒否）、`meta_entries`・`meta_layout`・`meta_write`（`meta_place` を分けた）、`ghci_vk_meta_table`・`ghci_vk_meta_bind`・`ghci_vk_meta_step`（`vk-execute.c` の `meta_run` もこれを使う）。
- `display.c`: `copy_engine` が上の表で選ぶ。`buffers_take` は blitter か program が写す時に 2 つの frame を session 0 に map（書ける）。`copy_by_program` は storage を session 0 に読むだけで map し、`copy_rows` の ticket を待ってから unmap する（blitter の道と同じ）。fence の後に flip を記録する。
- table の書き直し: display は 1 度に 1 つしか写さず（`copy_frame_busy`）、各写しの ticket を待ってから次へ進む。待ちが時間切れの後に次の写しが table を書き換えても、table が指す storage・frame の mapping は ring がその写しを過ぎるまで unmap されない（unmap は ring の順）ので、memory を外れて読み書きする事は無い。

## 実装（2026-11-13 P4）

- `src/drivers/gpu/ghci/ghci.h`、`display.c`、`display-scan.c`（comment）、`vk/vk.h`、`vk/vk-meta.c`、`vk/vk-execute.c`（`meta_run` を共有の関数に）、`vk/vk-node.c`（op・lock・detach）。
- 範囲に足した事: `pci-attach.c`（option を外し検出だけに）、`plan/ws347/tests/config-vf2-ghci.mk`（2 つの key を外した）、`vk/vk-caps.c`（shortfall から CLIP_POLY を外した）。
- 試験: `plan/ws290/tests/ghci-vk-host-test.c` に `scene_display_copy`、`ghci-driver-host-test.c` の「blitter 無し」の場面の名前（executor 無し、processor の道）。
- 台帳: `ghci-rtl-questions.md` の B32。

## 試験（2026-11-13 P4）

- host 試験 `sh plan/ws290/tests/ghci-host-test.sh`（worktree、log は `build/p008-host-test.log`）: words 151・ref 56・model 280・driver 1086・gsi1 50（compiler の PASS の行を含む）・vk 522・pipeline 176・execute 291・meta 98（GSI1 の model で 22 dispatch）、全部 0 failed、`PASS ghci290-host-test`。
  p003 の後の件数（vk 474）から vk が 48 増えた。足した場面（`scene_display_copy`）:
  - kc705-min・no-blt・kc705-discrete から CAP_BLT を外した物（local の frame、窓あり）: claim で 2 つの frame が session 0 に map される。frame と同じ stride の present と 4096 byte の stride の present がそれぞれ DISPATCH 1 つ、COPY_BUFFER 0、present の直後に frame は storage の中身を持たない（processor は写していない）、窓は使われない。packet は session 0 で head の BARRIER → BIND_PIPELINE（置いた program）→ BIND_DESCRIPTOR_TABLE（置いた table）→ LOAD_CONST → DISPATCH → tail の BARRIER。table は storage を read only・frame の mapping、push 定数は 640 × 480 の word、行の stride。その dispatch を test が手で走らせると frame が storage の中身になる。storage の mapping は present の後に unmap の途中。
  - kc705-discrete（blitter）: COPY_BUFFER 1、DISPATCH 0（今どおり）。kc705-uma（blitter、host の frame）: frame は map されず processor が写す（今どおり）。
  - kc705-min で copy の program が拒まれた印を立てる: DISPATCH 0、program は置かれず、processor が写す。
  - driver の host 試験（executor を link しない）の「blitter 無し」の場面は、executor 無しの GPU の processor の道として今どおり PASS（名前を直しただけ）。
  - kc705-min の shortfall: CLIP_POLY を挙げず、TU_BILINEAR を挙げる（期待を直した）。
- kernel: `make PLATFORM=vf2 CONFIGMK=plan/ws347/tests/config-vf2-ghci.mk llvm-toolchain-cached` → `sysroot BUILD_JOBS=3` → `build/vf2/vmunix -j3`、
  `make PLATFORM=x86_64 CONFIGMK=plan/ws347/tests/config-amd64-ghci.mk` の同じ 3 つ。両方 rc 0、warning 0、GHCI の 42 file を compile（log は worktree の `build/p008-*.log`）。
  vf2 は comment だけの直しの後にもう一度 link した（`build/p008-vf2-vmunix2.log`、warning 0）。

## 未実施・残り

- 実機（VF2＋KC705）・QEMU は無い（p004）。session 0 の dispatch・barrier・fence の後の flip が実機の GPU で正しく動くか、1 frame の写しの時間（CPU の写しとの比較）は p004 で量る。program の dispatch は host の model では走らず（model は packet を記録するだけ）、test が手で走らせた。copy の program そのものは meta の試験で GSI1 の model を通っている。
- 割り込みと DMA の coherent は検出だけになった。VF2 の PLDA は message を取らず、bitstream に INTx の pin が無いので `drv_pci_device_allocate_irqs` が ENOTSUP か ENODEV を返して polled に落ちる見込み。function が INTx の pin を報告するのに配線されていない場合は割り込みを待ち続けるので、p004 の attach の log（「no interrupt offered」の行）で確かめる。
- `plan/ws290/tests/config-visionfive2-ghci.mk`・`plan/ws288/tests/config-visionfive2-ghci.mk` は外した 2 つの key を今も書く（他の WS の file。key は今は何にも効かない）。
- Q1 への案: blitter 有り・host の frame（unified）も blitter で写すと「GPU に写す engine があれば GPU が写す」に揃う。今回は受け入れ（blitter の profile は今どおり）に従い変えていない。
- Q1 への案: 台帳の B32（flat の discrete の address の振り分け）が決まれば、KC705 の minimal でも frame を local memory に置け、表示の読みと写しの書きが PCIe から外れる。
- program の拒否の後も、各 present は storage を session 0 に map してから ENOTSUP を受けて processor に移る（map と unmap が 1 回ずつ余分）。拒否は GPU の compile の失敗で、起きれば attach の後の最初の写しで分かる。
- 規約の全文の見直しは p005。
