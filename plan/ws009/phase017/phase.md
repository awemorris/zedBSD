<!-- awesome-plan project=zedbsd record=ws009-p017 -->

# ws009-p017: DOC-54 gpu-display.md: output・lease・present・wait・events・constraints・副作用

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: なし（骨格 4dfcce008 と照合の script が main にあること）。p010〜p021 は互いに独立で、並べて起動してよい
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- src/drivers/gpu/gpu.c: `gpu_display_ioctl`（query・mode・claim・release・present・wait の分岐）・`gpu_display_present_resource`・`gpu_display_ungated`・`gpu_display_admit`・`gpu_events_snapshot`・`gpu_events_ioctl`・`gpu_constraints_ioctl`・`gpu_console_end`・`gpu_close`（lease の後始末）・`gpu_poll`（POLLPRI）、`tty_console_input_hold`・`tty_console_input_unhold` を呼ぶ所
- include/uapi/gpu-display.h・gpu-scanout.h の comment
- src/kern/text-display.c `kern_text_end`

## 2. 作る物

- `docs/reference/gpu-display.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: display-inventory・display-events（EVENTS・QUERY）、display-scanout（CONSTRAINTS・PRESENT）、display-control・gpu-bars・ghci-display（CLAIM・PRESENT・RELEASE）、gpu-share（MODE）。

## 4. 注意

- present が返る時（採用まで待つ output）は output の flag で書き、driver の名前はこの file に書かない（gpu-drivers.md の表）。
- 最初の flip で text console が display を使わなくなる事（`kern_text_end`）、claim が console の keyboard の入力を止める事は code の今の振る舞いを書く。lease の後に console が戻ると約束しない（driver ごと。header の comment の「returns the output to the console」と code の差は結果の節と報告に）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-display.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-display.md plan/ws009/phase017/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30（host の `date`）。担当: Sonnet 5.5 high。`docs/reference/gpu-display.md` を書いた（骨格 384 行 → 417 行）。cleared は Q1 が決める。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-display.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 18 capset rows, 1046 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (... final 0 files)`
- `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (309 links)`
- `git diff --check`: 出力なし（終了 0）。

### 自分で決めた点

- `GPU_DISPLAY_CONSTRAINTS` の Requires は `open mode none; capability GPU_CAP_DISPLAY`（`gpu_constraints_ioctl` は readable・writable を見ない。`scanout->constraints` の有無は bit でないので Errors の `EOPNOTSUPP` に書いた）。`GPU_DISPLAY_EVENTS` は `R; GPU_CAP_DISPLAY_EVENTS`（`gpu_events_ioctl` は `GPU_CAP_DISPLAY` を見ない。登録の検査が display を要求）。他の 6 つは `GPU_CAP_DISPLAY`。QUERY・MODE・WAIT は R、CLAIM・RELEASE・PRESENT は W。
- present が返る時: どの output flag も「採用まで待つ」と「flip を仕掛けたら返る」を区別しない（下の食い違いの 2）。文書は、flag が教えるのは FIFO・VIRTUAL_CLOCK・ATOMIC_MODE_PRESENT の意味までとし、確実に知るには `GPU_DISPLAY_WAIT` を非 0 の `timeout_ns` で呼ぶと書いた。driver の名前は書いていない。
- `GPU_DISPLAY_ATOMIC_MODE_PRESENT` は、present の width・height・refresh_millihz が frame と一緒に mode を選ぶ印（venus の `display_frame` が 1 つの scanout command で geometry と image を変える所に基づく）。flag が無い output は「別の大きさの frame で mode が変わるとは約束しない」と書いた（ghci は flag が無いが frame の大きさで mode を選ぶ。約束しないので矛盾しない）。
- `GPU_DISPLAY_FIFO` の Meaning は libvulkan が FIFO present mode を出す条件に使う事に基づく（header に定義が無い）。
- `generation` が変わる条件は venus の `display_refresh`（topology event）と ghci の `topology_move`（接続）に基づき「接続・topology の変化」と書いた。mode の変更では変わるとは書いていない（header の spec の「mode」は code で確認できなかった）。
- `frame` の欄は、core が検査せず、この tree の driver のどれも読まない（`->frame` を grep、該当は無関係の構造体のみ）と書いた。
- sequence 番号は lease ごとに数え直される（venus の claim が 0 に戻すのを読んで確認した。他の driver も lease の開始で 0 に戻す形に見えるが、全部は未確認）と書いた。header の COMMIT の comment の「one count per output that never goes back」より狭い。
- Errors の `ENODEV`・`EINTR`: `gpu_session_enter` と `gpu_session_error_locked` から。`EBUSY`（同じ thread の入れ子）は利用者から到達しないので書いていない。装置・open の記録された error は Other values に書いた。
- Sources の試験: QUERY=display-inventory、MODE=ghci-display、CLAIM・RELEASE・PRESENT・CONSTRAINTS=display-scanout、WAIT=hvs-load（`GPU_DISPLAY_WAIT` を直に呼ぶ唯一の program）、EVENTS=gpu-admission（display-inventory・display-scanout も直に使う）。display-events・gpu-bars・display-control は phase.md の見当にあったが、display-events は Vulkan 経由のみで直に呼ばない。

### header・spec と code の食い違い（Bug の候補）

1. `gpu-display.h` の `struct gpu_display_commit` の comment は「releasing the lease or closing the open returns the output to the console」。code では最初の flip の後に `gpu_console_end` が `kern_text_end(NULL, 0)` を呼び、`gpu_console_ended` は戻らず、`kern_text_end` の header は "There is no way back"。release・close は console を戻さない（driver が `kern_text_resume()` を呼んでも、終わった console は描かない）。powervr の log も "the text console resumes"。文書は code の今の振る舞いを書いた。
2. 指示（spec 3 の「output flag が present の返る時を教える」）に当たる flag が無い。FIFO は全 driver が付け、VIRTUAL_CLOCK・ATOMIC_MODE_PRESENT は venus だけ。present は ghci・bcm2711 では「記録して core が adoption_wait」、powervr・mali・nvrtx・amdrdna では「表示後に返る」、i915 では「flip を仕掛けたら返る（wait が latch を待つ）」。
3. 「lease が他の open の物」の errno: header（WAIT）は EBUSY。venus は `display_find_lease` が EPERM、i915 の wait は EINVAL、他は EBUSY。文書の Other values に書いた。
4. `gpu_display_mode` で i915 の存在しない output は ENOENT（header は ESTALE・ENXIO のみ）。bcm2711 の claim の `plane_index` 非 0 は EBUSY（他は EINVAL）。
5. `GPU_DISPLAY_WAIT` の `timeout_ns` を 1 秒に切るのは driver（bcm2711・ghci）で、core は切らない。powervr・mali・nvrtx・amdrdna・venus・i915 の wait は timeout_ns を見ずに即答する（header の「one call waits at most a second; zero answers at once」と矛盾はしない）。

### 自己の照合の表

| 主張（節と要約） | 根拠 | 試験 |
| --- | --- | --- |
| QUERY: R、`GPU_CAP_DISPLAY`、reserved 非 0 は EINVAL | `gpu_display_ioctl`（capability・writing 判定・query 分岐） | display-inventory |
| QUERY: 入力は version・size・index のみ、他は core が 0 にする | 同 `kern_memset(&request.query)` | display-inventory |
| QUERY: COUNT_ONLY は count のみ。順序つきの reply は display_id・generation 非 0、index < count、でなければ EIO | 同 | display-inventory |
| QUERY: name の最後を NUL にする | 同 | 無し |
| MODE: R。display_id・generation 非 0、count・flags 0、operation が 2 値、validate は index 0 で width・height 非 0 | 同 mode 分岐 | ghci-display |
| MODE: validate の結果に 0 があれば EIO。enumerate の width 等は core が 0 にする | 同 | ghci-display |
| CLAIM: W。display_id・generation 非 0、lease 0 でなければ EINVAL。driver が lease 0 を返せば EIO | 同 claim 分岐 | display-scanout |
| CLAIM: copyout 失敗で release に戻す。成功で `display_claims++`・`tty_console_input_hold`・`kern_text_progress_end` | 同 | display-scanout |
| RELEASE: W。lease 0 は EINVAL。成功で hold を戻す。reply を書かない | 同 release 分岐 | display-scanout |
| close: hold を全部戻してから driver の close | `gpu_close` | 無し |
| PRESENT: W。flags は FIFO か FIFO|BLOB のみ。width*4 が 32 bit。format 2 値。stride | 同 present 分岐 | display-scanout |
| PRESENT: 非 BLOB は STORAGE、BLOB は BLOB か SCANOUT、offset と extent の検査 | `gpu_display_present_resource` | display-scanout |
| PRESENT: ungated は record の後 `adoption_wait`、sequence 0 は EIO | 同、`gpu_display_ungated`、`gpu_display_admit` | display-scanout |
| PRESENT: 成功してから reply の copyout。失敗しても frame は出ている | `gpu_display_ioctl` | 無し |
| WAIT: R。lease 非 0、outputs 0。core は timeout を見ない | 同 wait 分岐 | hvs-load |
| WAIT: 採用済みなら即答する driver、待つ driver（ghci・bcm2711） | 各 driver の wait | hvs-load |
| EVENTS: R、`GPU_CAP_DISPLAY_EVENTS`、flags・ack_sequence の組み合わせ、ack は観測済み以下 | `gpu_events_ioctl` | gpu-admission、display-inventory |
| EVENTS: copyout の後に observed・acknowledged を更新。gpu_ioctl_body で admission の外 | 同 | gpu-admission |
| POLLPRI: readable、capability、sequence > acknowledged、失敗は POLLERR | `gpu_poll` | display-inventory |
| CONSTRAINTS: capability と `scanout->constraints`、入力 0、reply の検査（EIO） | `gpu_constraints_ioctl` | display-scanout |
| 副作用: 最初の成功した flip（4 つの request）で一度だけ `kern_text_end(NULL, 0)`、戻らない | `gpu_console_end`、`kern_text_end`、`text-display.h` | 無し |
| 副作用: ENOTSUP は黙る、他の失敗は log のみ | `gpu_console_end` | 無し |
| Direction と Meaning（構造体 8 つ、全欄） | header の構造体の comment と上の code | - |

### Errors から選んだ 3 つの読み直し

1. `GPU_DISPLAY_EVENTS` の EINVAL（`ack_sequence` が受け取った sequence より新しい、または現在より新しい）: `gpu_events_ioctl` の `request.ack_sequence > session->topology_observed || request.ack_sequence > sequence` と合う。flags 0 で ack_sequence 非 0、flags が ACK で ack_sequence 0 も同じ関数の分岐と合う。
2. `GPU_DISPLAY_CONSTRAINTS` の EIO（reply の検査）: `gpu_constraints_ioctl` を読み直し、「未知の format の bit」を書き落としていたので足した。COPY は `GPU_CAP_RESOURCE` と `GPU_CAP_TRANSFER` の両方、FOREIGN は SHARED と `import_image`、alignment は 2 の冪、の条件が合う。
3. `GPU_DISPLAY_CLAIM` の EIO（driver が lease 0 を返す）と EFAULT（copyout 失敗で release に戻す）: `gpu_display_ioctl` の claim 分岐の `if (request.claim.lease == 0) return EIO;` と、`copyout` 失敗の rollback の `ops->release` と合う。

### 未確認・未実施

- driver 全部の errno の網羅は確認していない（Other values には、読んだ driver（venus・ghci・bcm2711・powervr・mali・nvrtx・amdrdna・i915）で見た値だけを書いた）。xd9・bootfb・capture の node は claim・present を読んでいない。
- `generation` が mode の変更で変わるかは確認できていない。
- 実機・QEMU の試験は行っていない（文書の仕事）。
- link の検査は noct の 1 本のみ。他の文書との整合（最初の flip は p014、lease の後の画面は p018・p019、待ちの errno は p010・p014〜p018・p021）は統合の Phase が照らす。
