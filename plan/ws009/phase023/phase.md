<!-- awesome-plan project=zedbsd record=ws009-p023 -->

# ws009-p023: DOC-54 統合: 全文書の最終の照合、二重の主張の照合、既存の文書の直し

Status: planned（2026-10-30 P1 の設計、q1701。p009 を節ごとに分けた物）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54）。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」
実装の担当: Sonnet 5.5 high（1 回の起動）
依存: p022 が main に merge 済み
骨格の基点: 4dfcce008

## 1. 読む物

- AGENTS.md の §4（担当の共通の規則）・§7（禁止）・§10（管理情報を書かない）。
- [docs/style.md](../../../docs/style.md) と [gpu-doc-rules.md](../gpu-doc-rules.md)（書き方・形・確かめ・終わりの条件の共通の規則。この phase.md と同じ重さの手順）。
- 自分の file の骨格の全文（各節の `<!-- spec: -->` が書く事の仕様）と [gpu.md](../../../docs/reference/gpu.md) の骨格（共通の規約の spec）。
- 着手の時に gpu-doc-rules.md 1 節の `git log 4dfcce008..HEAD -- ...` で基点の後の UAPI の変化を見る。
- p010〜p022 の phase.md の結果の節（食い違い・未確認・自分で決めた点）
- src/drivers/gpu/gpu.c（食い違いの解消に要る handler だけ）

## 2. 作る物

- `docs/reference/gpu*.md（食い違いの直しだけ）`
- `docs/reference/control-devices.md`
- `docs/architecture/kernel-and-hal.md`
- `docs/reference/visionfive2-gpu.md`
- `docs/reference/kernel-boot-parameters.md`
- この phase.md の「結果」の節、[ws.md](../ws.md) の Phase の表のこの行。
- 見つけた header・code の食い違い（Bug の候補）と、tree の他の comment と code の食い違い（例: userland/desktop/sessiond/seat.c の node の open の comment）の一覧を Q1 に送る。

## 3. 確かめの項目

- 骨格の全ての `PENDING` と `<!-- spec: -->` を埋めた（script の `--final`）。
- 全 file で `--final` と `--examples` が PASS。4 節の照合の結果を表で結果の節に。

## 4. 注意

- `--final`（全 file）で残る FAIL を直す。
- 並行の Phase が別々に書いた同じ主張を照らし、code に揃える: bit が node の寿命の中で変わるか（gpu-capabilities.md・gpu-drivers.md）、venus の JOB（gpu-capabilities.md・gpu-jobs.md・gpu-drivers.md）、最初の flip（gpu-submission.md・gpu-display.md）、lease の後の画面（gpu-display.md・gpu-display-control.md・gpu-drivers.md）、待ちの errno と上限（gpu.md の表・gpu-submission.md〜gpu-display-control.md・gpu-examples.md）。
- visionfive2-gpu.md の「`/dev/gpu0` は display、`/dev/gpu1` は render」は、番号が probe の順の空き番号である事（gpu.md）と合わせる（例として書くなら、そう明記する）。
- kernel-boot-parameters.md を編集するので、その file の開発の管理情報（WS・Phase・Queue の番号、日付の経緯）も外す（意味を変えずに言い換える）。
- control-devices.md の「There is no published current GPU object ABI to enumerate here.」を gpu.md への link と 1〜2 文に。kernel-and-hal.md の GPU の段（text console が firmware の framebuffer に描き続けるという文）を今の振る舞い（最初の flip の後、text console は display を使わない。gpu-display.md の User-visible side effects）に。visionfive2-gpu.md と kernel-boot-parameters.md から gpu.md へ link。
- 各 Phase の報告の header・code の食い違い（Bug の候補）を一覧にして Q1 に送る。文書は今の振る舞いのまま。
- 見出しの変更は禁止のまま（`--base`）。
- `noct plan/ws009/tests/check-completed-producer-docs.noct .` も流し、gpu*.md が status の違反を足さないことを確かめる（手前の照合で止まるなら、その最後の行を記録）。

## 5. 変えてはいけない物

- 2 節の file 以外の全部（gpu-doc-rules.md 5 節）。見出し（H1〜H4）と機械の列。照合の script。header・source・HAL・toolchain・共有の build。
- 不備を見つけたら直さずに止め、file・場所・足りない物を報告する。

## 6. 確かめの command

```sh
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008 --examples --final
python3 -I plan/ws009/tests/check-gpu-uapi-doc.py --base 4dfcce008
timeout 120 /home/awe/zedBSD-claude1/build/NoctLang/build-static/noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture
git diff --check
```

## 7. 終わりの条件

gpu-doc-rules.md 6 節。commit は `git commit -m WIP -- docs/reference/gpu*.md docs/reference/control-devices.md docs/architecture/kernel-and-hal.md docs/reference/visionfive2-gpu.md docs/reference/kernel-boot-parameters.md plan/ws009/phase023/phase.md plan/ws009/ws.md`。
加えて: `--final` を file の指定なしで（全 file）PASS、`--examples` で amd64 の compile が 1 回以上。

## 結果

（未実施）

## p014 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- `GPU_COMMAND_SUBMIT` は driver を呼ぶ前に `sequence` を写し出すので、driver が断ると何も指さない sequence が残り、その wait は ENOENT。
- device の失敗の poll は POLLERR だけ、POLLHUP は device が外れた時だけ（skeleton の spec は POLLERR|POLLHUP だった）。UINT64_MAX の wait は device が外れると ENODEV。
- `GPU_COMMAND` の大きすぎは EINVAL、`GPU_COMMAND_SUBMIT` は E2BIG（header は無言）。`GPU_PRESENT` の不明な handle は ENOENT でなく EINVAL。

## p017 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- gpu-display.h は lease の release・close で output が console に戻ると書くが、最初の flip で `gpu_console_end` が `kern_text_end` を system で 1 回呼び、戻らない（powervr は「the text console resumes」と log する）。
- PRESENT がいつ返るかを示す output の flag が無い（driver ごとに採用後・表示後・flip の arm 後で違う）。
- 他の open の lease の WAIT の errno: header は EBUSY、venus は EPERM、i915 は EINVAL。i915 の mode の ENOENT、bcm2711 の plane_index の EBUSY（他は EINVAL）。
- `timeout_ns` の 1 秒の上限は core に無く bcm2711・ghci だけ、他の driver の wait は timeout を見ない。

## p011 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- i915 の capset は capset_id・version を見ず、どの id にも同じ payload を返す（他の driver は 4/0 以外を ENOTSUP）。
- `venus_get_info` が strict の queue が無い時に JOB・JOB_CAPACITY を落としても、`gpu_info_ioctl` が `ops->capabilities` で上書きするので GET_INFO は立てたまま、`GPU_JOB_RESERVE`・`CAPACITY` は ENOTSUP（venus.c の comment と食い違い）。
- Venus の kernel は host の capset がちょうど 168 byte の時だけ vendor の suffix を読み、libvulkan は 168 と 176 で読む（176 byte の host で strict が食い違う）。
- gpu-op.h の video の properties の comment は i915 だけと書くが、nvrtx と bcm2711（video 付きの build）も NATIVE_VIDEO_DECODE_H264 を立てる。
- i915 は契約が未実装（XXX）のまま vendor flags 7 を返す。

## p019 から（2026-10-30 Q1、Bug の候補の一覧に入れる）

- amdrdna の `present_present` の comment は flip の始まりで返ると書くが、code は flip を運ぶ update を待つ。
- `i915/display/present.c` の file の comment（output 1 つ、hot-plug・topology の event 無し）が code（複数の connector、GPU_DISPLAY_LIMITED、display の event）と食い違う。
- `CONFIG_DRIVER_PCI_AMDRDNA` の既定が config/drivers/pci.drivers は n、Makefile の amd64 は y。
- nvrtx・i915 は GPU_CAP_DISPLAY_CONTROL を立てるが GPU_DISPLAY_POWER に EOPNOTSUPP。
