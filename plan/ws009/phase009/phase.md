<!-- awesome-plan project=zedbsd record=ws009-p009 -->

# ws009-p009: DOC-54 `/dev/gpu` の UAPI と capability の reference

Status: planned → disposition canceled（2026-10-30 P1、q1701。2026-10-30 ユーザー「WS009、WS296・WS297は標準廉価実装手順を適用してphaseを作り替え、今から実行してOKです。ただ、1 WSずつ実施します。」により、標準廉価実装手順で節ごとの p010〜p024 に分けて置き換えた。この設計と review の指摘は p010〜p024 の骨格の spec と phase.md に移した。以下は第 2 版の記録）
Parent: [WS009](../ws.md)
Approval: 2026-10-27 ユーザーの回答（クリック）「今書く」（DOC-54 を保留から外した。ws.md の Status の原文）
依存: なし。並行の作業との merge の順は 8 節
見積もり: 22 LW（±50%）。2 つの段 p009a（12 LW）・p009b（10 LW）に分ける（review M11、capset の形は 2026-10-29 ユーザーの回答で +2）

## 1. 目的と成果物

- 新しい `docs/reference/gpu.md`（user の API の reference）。driver の内部の ops（`include/drivers/gpu/*.h`）は書かない（docs/style.md の architecture／reference の区分）。
- Status の行: `Status: experimental reference for GPU ABI version 1`（review M3: `plan/ws009/tests/check-completed-producer-docs.noct` 20〜27 行は current・experimental・deprecated・planned を求める。WS306・WS309 がまだ足しうる UAPI なので experimental）。
- 同じ変更で直す既存の文書（review M9、どれも WS009 の文書）:
  - `docs/reference/control-devices.md` 81 行「There is no published current GPU object ABI to enumerate here.」→ gpu.md への link に。
  - `docs/architecture/kernel-and-hal.md` 87〜90 行（text console が firmware の framebuffer に描き続けるという記述）を今の挙動（2 節の副作用）に。
  - `docs/reference/visionfive2-gpu.md`（`/dev/gpu0`・`/dev/gpu1` に触れる）と `docs/reference/kernel-boot-parameters.md` から link。index は `docs/reference/README.md`。
- docs/ から plan/ へは link しない（docs/style.md）。

## 2. 正とする source（main 61630bb0b。行は動くので関数の名前も書く、review m6）

| 物 | 中身 |
| --- | --- |
| `include/uapi/gpu.h`（262 行） | `GPU_ABI_VERSION` 1、capability の bit、'G' 0〜13 |
| `include/uapi/gpu-allocation.h`（69） | 'G' 14・15 |
| `include/uapi/gpu-fence.h`（107） | 'G' 16〜23 |
| `include/uapi/gpu-display.h`（426） | 'G' 24〜29・32・39〜45 |
| `include/uapi/gpu-scanout.h`（99） | 'G' 30・31・33 |
| `include/uapi/gpu-job.h`（101） | 'G' 34〜38 |
| `include/uapi/gpu-op.h`（2016） | command の stream。protocol の版 `GPU_OP_PROTOCOL_VERSION` は 2（review m1）。命令は `enum gpu_op` の要素 |
| `include/uapi/sysctl.h` | `hw.gpu.attaching`・`hw.gpu.start`（44〜56 行）・`hw.gputelemetry`（180〜220 行）、`hw.gpu.powervr.stages` |
| `src/drivers/gpu/gpu.c`（8116） | `gpu_open`（権限、1390〜1427 行）、`gpu_ioctl_body` の dispatch（2114 行〜）、`gpu_info_ioctl`（2523 行で `capabilities = device->ops->capabilities`）、登録の検査（1029〜1302 行）、console の終わり（`gpu_console_end`） |
| `src/drivers/gpu/gpu-fence.c`（626） | fence |
| `userland/desktop/sessiond/seat.c` | node の mode（既定 0666、`SEAT_GPU_MODE` 60 行）、seat の user に 0600 で渡すのは `/dev/gpu0`〜`gpu3`（47〜48・103〜104 行） |
| driver（10 file が `drv_gpu_register`） | i915・nvrtx・amdrdna・powervr（render と display）・bcm2711（render と display）・venus・ghci |

ioctl は 'G' 0〜45 の 46 個で抜け・重なりは無い（2026-10-29 に数え、review も確かめた）。

**capability の事実（review B1）**: `GPU_GET_INFO` が返す bit は `ops->capabilities`（gpu.c 2523 行）で、backend の `get_info` が書いた値は使われない。
例: venus の `VENUS_CAPABILITIES`（`venus/venus.c` 29 行）は 63（RESOURCE〜PRESENT）を含む。venus は strict queue が無くても `GPU_CAP_JOB` を返すが `GPU_JOB_RESERVE` は ENOTSUP（`venus/transport.c` 179・408 行）。
→ 文書は header と核の振る舞いを書き、この食い違いは文書で正しいと書かず Q1 に送る（Bug の候補、9 節）。

## 3. 文書の構成（p009a・p009b の担当の節）

p009a（共通の部分）:
1. Status 行・ABI の版・header の一覧（source への相対 link）。
2. 呼び方の規約: top level の要求の `version` は `GPU_ABI_VERSION`、`size` は ioctl ごとに決まる（表に書く）。例外を明記（review M6）: `GPU_COMMAND_SUBMIT_SYNC`・`GPU_DISPLAY_PRESENT_SYNC` は入れ子の struct の size（gpu.c 6971・6985 行）、`gpu_blob_create_placed` は外側の大きさ、`gpu_display_plane_state`・`gpu_placement` は version・size を持たない。固定幅、ILP32 と LP64 で同じ大きさ（review が 45 個の struct を x86_64 と -m32 で確かめた）。`-1` と `errno`。
3. node と権限（review M5）: `/dev/gpuN` の番号（`gpu%u`、gpu.c 1328 行）と使い回し、同じ device を見分ける `GPU_DEVICE_QUERY` の `device_id`、open の権限（root か node の owner）、node の mode（既定 0666、seat の user に 0600 で渡すのは gpu0〜gpu3）、**読みと書きの権限**（open の mode から `readable`・`writable`、gpu.c 1426〜1427 行）。ioctl の表に「要る open の mode（R・W・不要）」と「要る CAP」の列。
4. capability の model: 各 bit、bit が無い時の errno（`EOPNOTSUPP` が既定、gpu.c 2199〜2201 行と各 ioctl）、`GPU_GET_CAPSET`。**capset の payload の形も書く**（2026-10-29 ユーザーの回答、9 節）: id 4・version 0、168・176 byte、magic と flag の各欄。header の外の ABI なので、出典は driver（venus.c 735、amdrdna/runtime.c 55・293、nvrtx/runtime.c 42・195、powervr/render-runtime.c 50・185 など）と libvulkan の読む所。照合の script の対象に payload の欄の表を足す。p009a に +2 LW。
5. object の model と寿命: resource・blob・handle、mmap の offset、export・import の fd（allocation と image の descriptor、device_id は kernel が付ける）、scanout の import。
6. 投入と完了: `GPU_COMMAND`（受け付けは完了ではない）、`GPU_COMMAND_SUBMIT`・`WAIT`、fence（fence の ioctl が `GPU_CAP_FENCE` を要るかは `gpu_fence_ioctl` を読んで書く、review m7）、job（reserve・commit・cancel、`GPU_JOB_CAPACITY` の WAIT は flags 0、review m2、policy の値は build の config から）、session の失敗（device lost に当たる errno、回復の手順）。
7. **並行と待ち**（review M4）: 同じ fd で並べてよい ioctl（admission の外の物: `GPU_JOB_*`・`GPU_COMMAND_WAIT`・`GPU_DISPLAY_EVENTS`・`GPU_DISPLAY_REFRESH` など、gpu.c を読んで一覧に）、各 wait の timeout の 0・有限・`UINT64_MAX`・EINTR、取り消し。
8. 例: 普通の例（open → info → blob → submit → wait）と、失敗の例（timeout か session の失敗）（review M4）。

p009b（display と driver と直し）:
9. display: query・mode・claim と release・present と wait・events・power・refresh・cursor・commit・overlay、scanout の制約と placed blob。**user に見える副作用**（review M8）: 最初の flip で text console が framebuffer から外れる（`gpu_console_end`、2026-10-28 のユーザーの決定）、display の claim が console の keyboard の入力を止める、poll の意味。
10. command の stream（`gpu-op.h`）: 形（protocol の版 2、version 1 の番号は Venus の wire format 1（virglrenderer 1.1.0）と同じ、zedBSD 独自は `GPU_OP_OWN_FIRST` から）だけ。命令ごとの表は header を正とする（review m1）。
11. sysctl の GPU の項（`hw.gpu.*`・`hw.gputelemetry`）: 一覧と意味の要約、詳細は sysctl の文書へ link（review M10。推しの既定、9 節）。
12. node の種類ごとの表（review M12）: 行は render の node と display の node（bcm2711・powervr は 2 node、i915 は resident・bootfb・capture で bit が違う: display.c 958、bootfb.c 286、capture.c 219）。列は bit・bit が足される条件（amdrdna・nvrtx は runtime の bind で CAPSET・COMMAND・NOTIFICATION・JOB を足す: amdrdna/runtime.c 192、nvrtx/node.c 161。ghci は ghci.c 652〜671、vk/vk-node.c 129）・present が返る時を区別する display の flag（`GPU_DISPLAY_FIFO`・`VIRTUAL_CLOCK` など）。bit が node の寿命の中で一定かは、bind が `drv_gpu_register` の前か後かを読んでから書く（review の未確認）。
13. 互換: Linux の DRM とは別の ABI（「DRM compatible」と書かない）。

## 4. 範囲の外（推しの既定、9 節で確かめる）

- driver の開発用の制御の device（`/dev/amdrdna0`、`include/uapi/amdrdna.h` 16〜17 行の「It exists while the driver is being developed」）。
- 承認前の追加: WS306 のゲーム最適化の UAPI（設計の段）。WS250 は UAPI を変えない設計（plan/ws250/design.md「UAPI（`include/uapi/gpu*.h`）… を変えない」）で、変わるのは error と回復の振る舞い。文書は今の振る舞いを書き、WS250 の実装の時にその WS が直す（review M10）。

## 5. 試験と受け入れ（host だけ）

| 試験 | 合格 |
| --- | --- |
| 照合の script（新しい `plan/ws009/tests/check-gpu-uapi-doc.noct`、p009a で書く） | header から ioctl の組（名前・番号・向き・struct）を取り、文書の ioctl の表の行と組ごとに一致。gpu-op.h を除く `#define GPU_` の全部と `struct gpu_*` の全部が文書に現れる。逆向き（文書にあって header に無い名前）は `enum gpu_op` の要素と sysctl の `GPU_TELEMETRY_*` を対象に含めて誤りの FAIL を出さない（review M2） |
| 例の compile | 例を抜き出し、target の clang で `--target=x86_64-unknown-zedbsd --sysroot=<build>/amd64/sysroot -fsyntax-only`（`-I include` だけでは host の `<sys/ioctl.h>` の番号になる: include/uapi/ioctl.h 44〜54 行、review M1） |
| link | `noct plan/ws009/tests/check-relative-links.noct docs/reference docs/architecture`（引数は directory。`docs` 全体は今 `docs/agent/awesome-plan/README.md` の `img/1.png` が無く最初に FAIL する、review B2。この既知の失敗は Q1 に送る） |
| producer-docs | `check-completed-producer-docs.noct` が新しい文書の Status の行で違反を足さない |
| 出典の試験（DOC-T10、review B3） | 文書の各節が source tree の試験を出典に書く: `userland/tests/gpu-selftest`・`gpu-admission`・`gpu-recovery`・`gpu-share`・`display-control`・`display-inventory`・`display-scanout`・`acquire-fence` など（`userland/tests/` の実在を p009a の最初に確かめる）。plan/ の試験（`plan/ws014/tests/` は closed の WS の物）は出典にしない |
| 自己の照合 | 3 節の主張ごとの header・gpu.c の行と関数の表（phase の結果の節に置く。文書には置かない） |

受け入れ: 上が全部通り、DOC-T10・DOC-T50（current・deprecated・planned の区別）を満たす。ws.md の DOC-54 の行の Complete は Q1。

## 6. 見積もり（1 LW ≈ 実時間 15〜25 分）

| 段 | 作業 | LW |
| --- | --- | --- |
| p009a | header（gpu-op.h を除く約 1,060 行）と gpu.c の権限・dispatch・errno・並行の読み 4、文書 1〜8 節 4、capset の payload の形 2、照合の script と例の compile 2 | 12 |
| p009b | display の 9 節 3、10・11 節 1、node の種類ごとの表（9 種の node の ops と条件を読む）3、既存の文書の直し 1、link と producer-docs と照合 2 | 10 |
| 計 | gpu.md は 700〜1,000 行の見込み（evdev.md 205 行、kernel-boot-parameters.md 632 行から推測） | 22（±50%） |

節ごとに commit して Q1 に報告する。

## 7. 判断と選ばなかった案

- 置き場所は `docs/reference/gpu.md` の 1 本。display を別の file に分ける案は、番号空間と capability の model が共通なので採らない。
- command の stream の命令の表を写す案は、header が既に命令ごとの reference で二重の正になるので採らない。
- 書く担当は設計の担当（P5）ではない（P5 は docs/ を変えない範囲）。Q1 が割り当てる。

## 8. 並行の作業（review M10）

- WS291（P2、q1663）は gpu.c を変えている。WS309（P1、q1662）は複数の flip を queue に積む kernel の機能を扱う（ws309/ws.md）。WS242・WS250・WS306 は nvrtx・gpu の UAPI に触る。
- 着手の時と merge の前に `git log -- include/uapi/gpu*.h src/drivers/gpu/gpu.c` で変化を確かめ、変われば該当の節を直してから merge を頼む。

## 9. Q1 に送る点

- （Bug の候補、review B1）venus は strict queue が無くても `GPU_CAP_JOB` を返すが `GPU_JOB_RESERVE` は ENOTSUP（`venus.c` 29 行、`transport.c` 179・408 行）。
- （既知の失敗、review B2）`docs/agent/awesome-plan/README.md` の `img/1.png` が無く、link の試験を `docs` 全体で流すと FAIL する。
- 2026-10-29 ユーザーの回答（クリック、Q1 の中継）Q-D54-1: 「capset の形も書く」。→ 解釈: 範囲は `/dev/gpuN` の ABI 1、sysctl の GPU の項の要約、capset の payload の形（3 節の 4）。`/dev/amdrdna0` と WS306 は書かない（推しのまま）。
- (a) は Q1 が Bug にする（2026-10-29 Q1）。(b) は awesome-plan の固定版の欠けで触らない（下の既知の失敗）。
- 既知の失敗（2026-10-29 Q1）: `docs/agent/awesome-plan/README.md` の `img/1.png` が無いので、`check-relative-links.noct` を `docs` 全体で流すと FAIL する。この Phase は `docs/reference docs/architecture` を引数にする。
  （言語は docs/ の全文書と同じ英語で、聞かない。review m4）
