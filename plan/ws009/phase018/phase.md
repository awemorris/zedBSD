<!-- awesome-plan project=zedbsd record=ws009-p018 -->

# ws009-p018: DOC-54 gpu-display-control.md: power・refresh・cursor・commit・overlay

Status: in-progress（2026-10-30 書いた・照合 PASS。cleared は Q1 が決める。p009 を節ごとに分けた物）
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
- src/drivers/gpu/gpu.c: `gpu_display_ioctl`（power・cursor の分岐）・`gpu_display_cursor_resource`・`gpu_refresh_ioctl`・`gpu_commit_ioctl`・`gpu_commit_check`・`gpu_commit_plane_check`・`gpu_commit_record`
- include/uapi/gpu-display.h の comment（power・refresh・cursor・plane_state・commit・overlay_info）

## 2. 作る物

- `docs/reference/gpu-display-control.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 要求の節ごとに `Requires:`・本文・`Errors:`（最後に `- Other values:`）・`Sources:` の形（gpu-doc-rules.md 2 節の 6）。
- 構造体の表の Direction と Meaning を全行。定数の表の Meaning を全行。
- 自己の照合の表と、`Errors:` から 3 つの読み直し（gpu-doc-rules.md 4 節）。
- 出典にする試験の見当: display-control（POWER・REFRESH）、display-scanout（CURSOR_*）。COMMIT・OVERLAY_INFO は grep で確かめる。

## 4. 注意

- lease・generation・sequence の共通の意味は gpu-display.md（別の Phase）が書く。ここは link で済ませ、この要求に固有の事を書く。
- `GPU_DISPLAY_COMMIT` の DROPPED は all or nothing の唯一の例外。ETIMEDOUT の後の lease の振る舞いを header と code で確かめる。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-display-control.md
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu-display-control.md plan/ws009/phase018/phase.md plan/ws009/ws.md`。

## 結果

2026-10-30 10:13（host の `date`）。担当: Sonnet 5.5 high。

### 実行した command と最後の行

- `git log --oneline 4dfcce008..HEAD -- include/uapi/gpu*.h include/uapi/sysctl.h include/uapi/errno.h src/drivers/gpu/gpu.c src/drivers/gpu/gpu-fence.c userland/desktop/libvulkan/context.c`: 出力なし（基点の後の UAPI・gpu.c・gpu-fence.c・libvulkan の変化なし）。
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --final docs/reference/gpu-display-control.md`: `check-gpu-uapi-doc: PASS (14 files, 46 index rows, 46 request sections, 98 constant rows, 47 structures, 20 capset rows, 635 placeholders, final 1 files)`
- `python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008`: `check-gpu-uapi-doc: PASS (... final 0 files)`
- `timeout 120 .../noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`: `Markdown relative-link check: PASS (488 links)`
- `git diff --check`: 出力なし（終了 0）。

書いた行数: `docs/reference/gpu-display-control.md` は 343 行（骨格 307 行）。PENDING・`<!-- spec: -->` は 0。

### 自分で決めた点

- Requires: POWER は `W; GPU_CAP_DISPLAY and GPU_CAP_DISPLAY_CONTROL`（`gpu_display_ioctl` が `GPU_CAP_DISPLAY` を見てから POWER の分岐で `GPU_CAP_DISPLAY_CONTROL` を見る）。REFRESH は `R; GPU_CAP_DISPLAY_CONTROL`（`gpu_refresh_ioctl` は `GPU_CAP_DISPLAY` を見ない。登録の検査が `GPU_CAP_DISPLAY_CONTROL` に display を要求する）。CURSOR_SET・MOVE・HIDE は `W; GPU_CAP_DISPLAY`（cursor の op の有無は bit でないので Errors の `EOPNOTSUPP`）。COMMIT は `W; GPU_CAP_DISPLAY`（fence を使う時だけ `GPU_CAP_FENCE` が要るので本文と Errors に書いた）。OVERLAY_INFO は `R; GPU_CAP_DISPLAY`。
- OVERLAY_INFO: `gpu_ioctl_body` はこの request を ungated の道でしか `gpu_display_ioctl` に渡さないので、ungated を宣言しない driver は `GPU_CAP_DISPLAY` だけでは `EOPNOTSUPP`（`GPU_CAP_DISPLAY` だけで届くとは書いていない）。登録の検査は `commit` と `overlay_info` を一組にし、commit に ungated を要求するので、overlay_info を持つ driver は必ず ungated。
- nvrtx と i915 は `GPU_CAP_DISPLAY_CONTROL` を持つが、電源の無い output の POWER に `EOPNOTSUPP`（nvrtx は display・generation・lease の検査の後、i915 は eDP パネルでない resident output に lease の検査の前）と書いた。
- REFRESH: 1 秒の切り詰めは core、ETIMEDOUT・睡眠は driver。amdrdna・i915・nvrtx・venus は signal を見ずに寝る。timeout 0 で `cursor` 非 0 は ETIMEDOUT、`cursor` 0 は即答。POWER と cursor は ungated でない driver では admission を取る（EINTR）。REFRESH と COMMIT は admission の外なので EINTR を Errors に書かない（COMMIT の fence 待ちの EINTR は別に書いた）。
- COMMIT の ETIMEDOUT: record は待ちの前に済んでいる事、reply を書き戻さない事を code から書いた（header は「output が画像を持ち続け、後の commit と present が EBUSY」まで）。signal_fd は record・adoption の失敗で error 付きで signal され、待ちの fence・bind の失敗では触らない。
- plane の image は blob か scanout だけ（`gpu_commit_record`）。header の "storage of this open" より狭い（下の食い違いの 1）。
- driver 固有の事（bcm2711 の overlay 数 2、primary の矩形、overlay は primary の上、100 ms の adoption、bandwidth の ENOSPC、lease ごとに serial を数え直す）は「bcm2711」と名指して書いた。commit を持つ driver は bcm2711 だけ（`.commit =`・`commit = ` を grep）。
- COMMIT の `sequence` が lease ごとに数え直されるかは書いていない（bcm2711 の `state_serial` は lease で戻らず、cursor の変更でも進む。gpu-display.md は lease ごとと書く）。

### header・spec と code の食い違い（Bug の候補）

1. `GPU_DISPLAY_COMMIT` の plane の image: header の plane_state の comment は "storage of this open"。code の `gpu_commit_record` は blob か scanout 以外（ordinary storage）を `EINVAL` にする。
2. `GPU_DISPLAY_COMMIT` の comment の「ETIMEDOUT の後 lease の release か close が output を console に戻す」: 最初の flip の後の console は戻らない（p017 の結果の食い違い 1 と同じ。gpu-display.md の User-visible side effects に従い、文書は console が戻るとは書いていない）。
3. `gpu_display_plane_state` の `GPU_DISPLAY_PLANE_IMAGE` を立てて `GPU_DISPLAY_PLANE_SHOWN` を立てない state: core は通し、bcm2711 だけが EINVAL にする。header は何も言わない。
4. POWER の comment「EOPNOTSUPP when the output has no power control」の順: nvrtx は lease の検査の後、i915 は lease の検査の前（header は順を言わない）。「EBUSY: another open's lease, or none」に加え、i915 は lease の frame がまだ出ていない間も EBUSY。
5. CURSOR_SET の comment は「EOPNOTSUPP without GPU_DISPLAY_HAS_CURSOR」。core は op の有無（`cursor_set == NULL`）で EOPNOTSUPP にし、flag は見ない。nvrtx は op があり flag が無い output で自分で EOPNOTSUPP。
6. REFRESH の comment「ENODEV when the GPU is offline」: core は offline の ENODEV の他に、device と open の記録された error も返す。
7. `time_ns` の「1 ms monotonic scheduler clock」: `HAL_TIMER_FREQUENCY` は 1000 の arch を確認した（`mips32.h`・`mips64.h`・`arch.h`・`armv7.h`）。他の arch は全部は未確認。

### 自己の照合の表

| 主張（節と要約） | 根拠（file と関数） | 試験 |
| --- | --- | --- |
| 序: POWER と REFRESH だけが `GPU_CAP_DISPLAY_CONTROL` を要る | `gpu_display_ioctl`（power 分岐）、`gpu_refresh_ioctl`、登録の検査（power と refresh は一組） | display-control |
| 序: kernel は output の flag を読まない | `gpu_display_ioctl`・`gpu_commit_ioctl`（flag の参照なし） | 無し |
| Refresh boundaries: count は戻らず、`cursor` 非 0 の返答は `cursor` より大きい（でなければ EIO） | `gpu_refresh_ioctl` | display-control |
| Refresh boundaries: venus は `GPU_DISPLAY_VIRTUAL_CLOCK`、他は flags 0 | `display_refresh_boundary`、`present_refresh`、`i915_display_refresh`、`present_refresh_wait` | display-control |
| Refresh boundaries: 電源 off の output は boundary を出さず ETIMEDOUT（amdrdna・venus） | `present_refresh`、`display_refresh_boundary` | display-control |
| Refresh boundaries: i915 の他の connector は boundary を出さない | `i915_display_refresh`（`lit_possible`）、`drv_i915_display_refresh_wait` | 無し |
| Refresh boundaries: cursor の変更は次の boundary から | header、`bcm2711_display_flip_cursor`、`present_cursor_move` | display-scanout |
| Planes: plane 0 は primary、overlay は最大 `GPU_DISPLAY_OVERLAY_MAX`、commit は最大 5 | `gpu_commit_check`、`gpu_commit_plane_check` | hvs-load |
| Planes: plane の二重指定は EINVAL、`count` から後の entry は 0 と -1 | `gpu_commit_check` | hvs-load |
| Planes: IMAGE の無い state は handle 0 と wait_fd -1 | `gpu_commit_plane_check` | hvs-load |
| Planes: image は blob か scanout、extent を検査 | `gpu_commit_record` | hvs-load |
| Planes: SHOWN の無い IMAGE は bcm2711 が EINVAL、overlay は primary の上 | `view_changes_locked` | 無し |
| Planes: serial 0 は EINVAL、serial が下がれば DROPPED、全部 DROPPED なら番号は進まない | `gpu_commit_plane_check`、`view_changes_locked`、`bcm2711_display_flip_commit` | 無し |
| Planes: lease の始めに plane は hidden、serial を数え直す（bcm2711） | `bcm2711_display_flip_own` | 無し |
| Planes: wait_fd は全部を record の前に待つ。他の GPU の fence も可。EAGAIN・EINTR・fence の error | `gpu_commit_ioctl`、`gpu_dependency_wait`、`fence_wait` | 無し |
| Planes: signal_fd は COMMIT_WAIT と共に、同じ device の fence、失敗では error で signal | `gpu_commit_check`、`gpu_fence_get`、`gpu_fence_binding_end` | 無し |
| Planes: fence を使うと `GPU_CAP_FENCE` が要る | `gpu_commit_ioctl` | 無し |
| Planes: COMMIT_WAIT は primary を含む。adoption を待ち adopted ≥ sequence | `gpu_commit_check`、`gpu_commit_ioctl`、`output_adoption_wait` | hvs-load |
| Planes: ENOSPC は bandwidth・clock・overlay の上限、何も変えない | `bcm2711_display_flip_commit`、`check_overlay` | 無し |
| Planes: ETIMEDOUT は record の後、reply を書かず、後の commit は EBUSY（uncertain） | `gpu_commit_ioctl`、`bcm2711_display_flip_wait`、`bcm2711_display_flip_commit` | 無し |
| POWER: 3 つの state、reserved 0、書き戻さない、admission を取る | `gpu_display_ioctl` | display-control |
| POWER: SUSPEND は OFF と同じ（amdrdna・i915・venus）、同じ state は成功 | `present_power`、`i915_display_power`、`drv_i915_display_power_set`、`display_power` | display-control |
| POWER: lease の終わりで on に戻る | `present_power` 周辺、`drv_i915_display_power_restore_locked`、venus の release | display-control |
| POWER: nvrtx は lease の検査の後 EOPNOTSUPP、i915 は eDP 以外に lease の前 | `present_power`（nvrtx）、`i915_display_power` | 無し |
| REFRESH: R、lease 不要、flags・sequence・time_ns は 0、1 秒に切る、admission の外 | `gpu_refresh_ioctl`、`gpu_ioctl_body` | display-control |
| CURSOR_SET: 幅高さ 1〜64、stride、format、extent は `stride × (height − 1) + width × 4` | `gpu_display_ioctl`、`gpu_display_cursor_resource` | display-scanout |
| CURSOR_SET: copy 済みで借りない。同じ rows は copy しない driver もある。EBUSY は一時 | header、`output_cursor_set`（bcm2711）、`display_cursor_set`（ghci）、`nvrtx_cursor_set` | display-scanout |
| CURSOR_MOVE: image が無ければ EINVAL、hidden を表示する | `present_cursor_move`、`output_cursor_move`、`display_cursor_move` | display-scanout |
| CURSOR_HIDE: image は残る。二重の hide は成功 | `present_cursor_hide`、`output_cursor_hide`、`display_cursor_hide` | display-scanout |
| COMMIT: signal_fd の EINVAL の組み合わせ、COMMIT_WAIT の primary、flags | `gpu_commit_check` | hvs-load |
| COMMIT: EBADF・EXDEV・EAGAIN（bind の枠）・ESTALE・EBUSY（bind） | `gpu_fence_get`、`drv_gpu_fence_device`、`gpu_fence_binding_add`、`drv_gpu_fence_bind` | 無し |
| OVERLAY_INFO: R、plane は 1 〜 `GPU_DISPLAY_OVERLAY_MAX`、output 欄は 0、ungated の道だけ | `gpu_display_ioctl`、`gpu_ioctl_body`、`gpu_display_ungated` | 無し |
| OVERLAY_INFO: bcm2711 の limits と ENXIO・EINVAL | `output_overlay_info`、`find_output` | 無し |
| Direction と Meaning（構造体 8 つ、定数 14 個、全欄） | header の comment と上の code | - |

### Errors から選んだ 3 つの読み直し

1. `GPU_DISPLAY_REFRESH` の `EIO`（`sequence` が非 0 の `cursor` より大きくない）: `gpu_refresh_ioctl` の `if (cursor != 0U && request.sequence <= cursor) return EIO;`（driver を呼んだ後、copyout の前）と合う。`cursor` 0 では検査しない事、`flags`・`sequence`・`time_ns` が非 0 なら EINVAL の事も同じ関数の分岐と合う。
2. `GPU_DISPLAY_COMMIT` の `EINVAL`（COMMIT_WAIT が primary を含まない、signal_fd が COMMIT_WAIT なし）: `gpu_commit_check` を読み直し、`primary == 0U` の検査と、`signal_fd >= 0 && (flags & WAIT) == 0` の検査が書いた通りの条件である事、`count` から後の entry の検査が `plane`・`flags`・`handle`・`wait_fd != -1`・`result` である事を確かめた。
3. `GPU_DISPLAY_OVERLAY_INFO` の `EOPNOTSUPP`・`EINVAL`: `gpu_display_ioctl` の分岐で、`ops->overlay_info == NULL` が EOPNOTSUPP、`plane == GPU_DISPLAY_PLANE_PRIMARY || plane > GPU_DISPLAY_OVERLAY_MAX` と output 欄の非 0 が EINVAL。ungated を宣言しない node では `gpu_ioctl_body` の switch に case が無く `default` の EOPNOTSUPP になる（その前に admission を取るので EINTR・ENODEV はありうる、と書いた）。

### 未確認・未実施

- driver 全部の errno の網羅は確認していない。Other values には amdrdna・i915・nvrtx・venus（power・refresh）、amdrdna・nvrtx・ghci・powervr・bcm2711（cursor）、bcm2711（commit・overlay）で見た値だけを書いた。mali・powervr の power・refresh はそもそも op が無い。
- cursor の op を持つ driver は grep で amdrdna・nvrtx・ghci（cursor 付きの table）・powervr・bcm2711 だけ（venus・i915・mali には無く、core が EOPNOTSUPP）。powervr の cursor_set が scanout の無い間に返す EOPNOTSUPP は Other values に具体的には書いていない。
- `time_ns` が全 arch で 1 ms の刻みかは全部は未確認（上の食い違い 7）。
- OVERLAY_INFO を直に呼ぶ試験は tree に無い（`no source-tree test`）。実機・QEMU の試験は行っていない（文書の仕事）。
- 他の文書との整合（最初の flip は p014・p017、lease の後の画面は p017・p018・p019、待ちの errno は p010・p014〜p018・p021）は統合の Phase が照らす。
