<!-- awesome-plan project=zedbsd record=ws322-design-review -->

# WS322 design review 1（design-reviewer、2026-11-01）

- 対象: [design.md](design.md) 第 1 版（P2、q1845）。review は読むだけで行い、実機・QEMU は使っていない。
- 結果: 27 件（blocker 1、major 9、minor 17）。全部を第 2 版に反映した（下の表）。review が確かめて正しいとした事: §2 の各行と行番号、§3.1 の address・IRQ・PMU の bit・clock と reset の番号と親、§3.2 の Wave5 の定数、`FORCE_FB_LATENCY_PLUS1` の値、CREATE_INSTANCE と SET_FB の段、HAL の変更が要らないという主張、出典の書き方、Q322-2 をユーザーに回す扱い、encode の header の前提。
- review が未確認とした事（第 2 版でも未確認、p002 の probe で確かめる）: VPU の DMA が L2 を通るか、uncached の読みの実の速さ、Wave511 が `BS_RD_PTR` の書き込みを受けるか、Vulkan の spec の返り値と VUID の正確な文面。

| # | 重さ | 指摘（要点） | 反映 |
| --- | --- | --- | --- |
| 1 | blocker | Wave5 の decoded・display の index は FBC と linear の組の 1 つの空間（wave5-vpuapi.c:513、wave5-vpu-dec.c:430-442,365,1794）。linear を 1 枚だけ空けると、参照を持つ絵で空きの FB が無くなる | §7.4・§7.5 を書き直した: N = 求める数 + 1 の組を登録、Vulkan の slot と index を結び、参照に使われていない index i の linear を SET_FB の option 1（1 枚の更新）で宛先に差し替え、display の flag で i だけを使わせる。更新が使えなければ driver の pool と GPU の transfer。R2・R3・D3・§0・T3・probe の項目も直した |
| 2 | major | 電源の on・off と `operations()` が食い違い、video が出ない | §7.1 で確かめの状態と電源の状態を分け、attach で on → product code → firmware → boot、v1 は on のまま。§10.4・D10 |
| 3 | major | DISTINCT の道はどの実の driver でも走っていない。vkvideo-probe は COINCIDE だけ | §12 に事実と持ち主、p004 に vkvideo-probe の DISTINCT 対応（+LW）、Q322-7（libmedia の直しの持ち主） |
| 4 | major | readback の速さ（uncached、cached の type 無し）で 1080p60 は届かない見込み | R13、p007 の受け入れを decode の時間と 1080p30 に、1080p60 は WS318 p009 の後の p012。Q322-3 の推しを直した |
| 5 | major | sequence の変化で IDR を捨てる、IDR だけを待つと recovery point の stream が戻らない、RESET のたびに pool を取り直す | INIT_SEQ の後と sequence の変化の後に同じ絵を decode し直す、INIT_SEQ は I slice の絵ならどれでも、RESET は SPS が同じなら FLUSH だけ（§7.4・§7.6・§11） |
| 6 | major | DST の retain の持ち主が無い、timeout の後の memory の保持が close を越えない | retain は executor が持ち、engine は鍵を返すだけ（§8.1・§9.1）。timeout は `decode` が返る前に reset の line を assert して VPU を止める（§9.1・§11） |
| 7 | major | VPU の DMA が L2 を通るかを確かめていない（`dma-coherent` と vendor の L2 の書き戻し） | §9.2 に未確認と明記、p002 の probe に一貫性の試験（R14）、通るなら `kern/dcache.h` で書き戻す。§3.1 の参照の誤り（§9.3 → §9.2）を直した |
| 8 | major | NV12 の plane 1 の行が 32 に丸められず transfer が外を読む | §8.4: plane 1 も 32 行に丸め、`layout.bytes` に含める |
| 9 | major | 実装の Phase の前倒しはユーザーの決定の範囲か | Q322-6 を足した（§15・§18・phase.md） |
| 10 | major | 失敗の道と並行性の試験が無い | §14.1 の T7、§14.3 の失敗の道の項目（fault の注入、reset と他の session、close、同時の decode、memory の type の両方）、p004 の受け入れ |
| 11 | minor | FB の上限は 31、数は求める数 + 1 | §3.2・§7.4 |
| 12 | minor | linear の stride は登録全体で 1 つ | §7.4（pool の pitch を DST に合わせる）、§7.5 の 3.（pitch が違えば pool の道） |
| 13 | minor | read pointer の戻しに SET_BS_RD_PTR の query がある | §7.3（正の道にし、probe で両方） |
| 14 | minor | DESTROY は finish_seq と同じ命令、再試行、CREATE_INSTANCE の完了の待ち方 | §7.1・§7.2 |
| 15 | minor | 割り込みの handler の手順、domain が off の時の読み | §10.2 |
| 16 | minor | NOC・VDEC_MAIN の reset、AXIMEM に clock は無い | §3.1・§20 |
| 17 | minor | H.265 の 4096×2304 は level 5.1 の MaxLumaPs を超える | §3.2 で H.265 は 4096×2176 |
| 18 | minor | `VK_ERROR_TOO_MANY_OBJECTS` は返せない | §7.2（INITIALIZATION_FAILED・OUT_OF_DEVICE_MEMORY） |
| 19 | minor | NV12 に SAMPLED_IMAGE を出すなら YCbCr の bit が要る | §8.2: NV12 の features に SAMPLED は出さず、plane の view（EXTENDED_USAGE）で満たす |
| 20 | minor | DPB だけの image の 4 KiB は bind の検べも変える | §8.4 |
| 21 | minor | DMA engine の copy は遅く、guardrail の文言の外 | DMA engine をやめ、pool を session の空間に map して GPU の transfer（§7.5・§10.3・§19・D3） |
| 22 | minor | LW の計算の誤り（encode を外すと 94）、p002 が楽観的 | §15 を直した（p002 20、p004 24、合計 152、encode を外すと 102） |
| 23 | minor | T1 は FMO・ASO を除き、byte でなく値で比べる | §14.1 |
| 24 | minor | decode を `build.sent` に数える、p007 は render-queue.c を触る | §8.5・§15・§20 |
| 25 | minor | `-2`（skip）と `-1`（FB 無し）を区別 | §7.6 |
| 26 | minor | `vkGetEncodedVideoSessionParametersKHR` は最初の encode の前に呼ばれ得る | §13: header は `vpu-h265-ps.c` で software で書く案を推す |
| 27 | minor | phase.md の状態、ws.md の「原文」の節が AGENTS §8 に反する | phase.md を in-progress にした。ws.md の原文の節はこの Phase の作った物でないので Q1 に知らせる |
