# AAT の道具（`plan/tools/aat/`、WS173 p003）

エージェントが host から SSH で zedBSD の desktop を人のように操作して受け入れを確かめる（Agent Acceptance Test）ための道具。
素の Latitude 5330（AAT の image をユーザーが毎回 USB から起動し、エージェントは起動の後に SSH で入る。2026-10-05 ユーザーの決定）と、`plan/tools/guest/guest.py` の QEMU の guest（`--qemu`）に同じ命令が効く。

| file | 役目 |
| --- | --- |
| `aat` | CLI（python3）。下の命令 |
| `config-amd64-aat.mk` | AAT の image の config: UAT の config（`plan/ws159/tests/config-amd64-uat.mk`）＋試験の注入（`CONFIG_INPUT_TEST_INJECT`、`aat-input`）＋撮影の口（`ZEDBSD_TEST_SCREEN_CAPTURE`、`keiland-shot`）。製品の config には入れない |
| `build-image.sh` | AAT の image を作る（root の鍵と host 鍵は guest の harness の物、harness の `net.conf` は入れない） |
| `tests/run-host.sh` | host の自己試験（`--local`、偽の `aat-input`（`fake-aat-input.py`）と偽の撮影 `fake-shot.py`） |

## シナリオと runner（ws173-p004）

シナリオの本体は `tests/scenarios/` の文書（[plan/tests.md](../../tests.md)）。エージェントは文書を読み、下の `aat` の命令で操作して確かめる。自動の補助（任意）は同じ id で `scenarios/` に置く。

| file | 役目 |
| --- | --- |
| `run-aat.sh TARGET OUTDIR [SUITE\|ID\|PATTERN\|area:NAME ...] [--record PATH] [--no-samples]` | suite（既定 `smoke`）か id を順に流す。TARGET は `5330`・`qemu`・`user@host[:port]`。前に `aat check`・session の `ZWL READY`・`aat start`・試験の file。各シナリオは補助があれば補助が流し、無ければ `by-agent`（エージェントが文書で行う）、`human: hands` は `needs-person`、QEMU の `machine: hardware` は `not-run`。`OUTDIR/summary.md` が実行の記録（`--record plan/ws173/runs/<日付>-<suite>.md` で写す） |
| `check-scenarios.py [--list]` | 文書と suite の形（header・節・各操作の 4 項目・path の実在・suite の各行）を確かめる |
| `scenarios/helpers_os.py`・`helpers_desktop.py`・`helpers_apps.py` | 補助（`--list` で持つ id）。各 step の action・見た物・撮影を `OUTDIR/records/ID.md`、判定を `OUTDIR/verdicts.tsv`、その scenario の間の session の log を `OUTDIR/logs/ID.log`（q788） |
| `scenarios/aatlib.py`・`common.py` | 補助の共通: App Home からの起動（`ZWL HOME icon` の位置を click）、窓（`aat windows`）、Settings の頁と control（`ZSETTINGS CONTROL`）、入力方式、前後の片付け（app を閉じる、`/etc/shadow` を戻す） |
| `scenarios/samples.py` | 試験の file（PNG・JPEG・2 頁の PDF・4 秒の MP4）を host で作り target の `/tmp/aat-samples` に置く（image に入れない） |

判定: `pass`・`fail`（どの操作で何が違ったか）・`needs-person`（撮影を人が見る `look`、人の手 `hands`）・`by-agent`・`not-run`。

## 使い方

```sh
plan/tools/aat/build-image.sh build/aat                 # 5330 用（USB に書く: build/aat/hdd-image.img）
export AAT_TARGET=5330                                   # root@10.0.30.3（--target user@host[:port] でも）
plan/tools/aat/aat check                                 # 届くか、注入・撮影の命令と log があるか
plan/tools/aat/aat start                                 # aat-input の server を起こす（画面の大きさは撮影から）
plan/tools/aat/aat type kei; plan/tools/aat/aat key enter    # 例: greeter で login
plan/tools/aat/aat mark login
plan/tools/aat/aat wait-log 'ZWL MAP' --since login --timeout 30
plan/tools/aat/aat windows                               # compositor の ZWL の行から窓の一覧
plan/tools/aat/aat click 640 400; plan/tools/aat/aat drag 100 100 400 300
plan/tools/aat/aat wheel 640 400 -3; plan/tools/aat/aat key ctrl+alt+t   # wheel は N > 0 で上
plan/tools/aat/aat shot build/aat-shots/step1.png        # 画面を手元に
plan/tools/aat/aat stop
```

- 鍵: `aat` は自分の checkout の `plan/tmp/guest/id_ed25519`（guest の harness の鍵、`guest.py keys` が作る、git に入らない）を使う。image は**同じ checkout** で作る（`build-image.sh` がその公開鍵を `/root/.ssh/authorized_keys` に入れる）。別の鍵は `--identity`。
- 接続は 1 本を使い回す（ssh の ControlMaster）。2 回目からの命令は速い。control の socket は短い `/tmp/aat-<uid>/ssh-%C`（Unix の socket の path の上限、T1-200）、mark と画面の大きさは `build/aat/<target>/`（`AAT_STATE`）。
- 座標は画面の pixel（左上が 0,0）。`move`・`click`・`drag`・`wheel` は `aat-input` の絶対の pointer（`move-to`）。`rel` は相対の mouse の量（compositor の加速がかかる）。`drag` は左 button だけ（`aat-input` の drag）。他の button で引くなら `move`・`down`・`move`・`up`。
- touch screen（ws190-p002）: `tap X Y [--count 2]`（2 で double tap、間 120 ms）・`touch-drag X1 Y1 X2 Y2 [--steps N]`（1 本指、離して終わる）・`touch-down ID X Y`・`touch-move ID X Y`・`touch-up ID`（指 0〜9 を持ったまま。long press は `touch-down`・待ち・`touch-up`）。`aat-input` が 4 つ目の device（`INPUT_INJECT_KIND_TOUCH`、画面の pixel）を持つので、inject の口（`INPUT_INJECT_OPENS_MAX` 4）は aat-input の間 `touchinject`・`peninject` に空かない。
- `key` の名前は evdev の名前の小文字（`enter`・`leftctrl`・`f5`、`ctrl`・`alt`・`shift`・`super` は左）。`type` は ASCII を US の配列で打つ（改行は `key enter`）。日本語は IME（`key` で切り替えて仮名を打つ）か貼り付けで。
- log の既定は `/run/user/1000/session.log`（`AAT_LOG`、`--log`）。`mark NAME` で今の長さを覚え、`--since NAME` でその後だけを見る。UTF-8 は host で解く（guest の shell を通さない）。
- `windows`・`where` は `ZWL MAP`・`ZWL UNMAP`・`ZWL WINDOW centred`・`ZWL RESIZE settled`・`ZWL RESIZE end`・`ZWL GLASS press move` の行から、surface ごとの最後の位置と大きさ。app の名前・題名・titlebar の部品の位置は今の log に無い（下の「P1 への依頼」）。
- QEMU と実機の証拠は分けて書く。QEMU の guest の判定に console・serial の log を使わない（AGENTS.md）。この道具は SSH と撮影だけを使う。

環境変数: `AAT_TARGET`、`AAT_LOG`、`AAT_INPUT`（既定 `/bin/aat-input`）、`AAT_SHOT`（撮影、既定 `/bin/keiland-shot {path}`）、`AAT_RUN_DIR`（target の撮影と `aat-input start` の出力の一時の置き場、既定 `/tmp/aat`）、`AAT_STATE`（host の状態、既定 `build/aat`）、`AAT_PASSWORD`（root 以外で入った時の sudo、既定 kei）。

## target の側の口（P1、ws173-p001・p002）

- 入力: `aat-input`（`userland/tests/aat-input`、root）。`aat-input start --width W --height H` が `/dev/input-inject` に相対の mouse・絶対の pointer（W×H の画素、既定 1920×1200）・keyboard・touch screen（同じ画素、ws190-p002）を宣言して背景の server になり（`/run/aat-input.sock`）、`AAT-INPUT ready` を出す。`aat-input COMMAND ...` は 1 つの命令を送り `ok` か `error WHY`。命令は `move-to X Y`・`move DX DY`・`click [BUTTON] [X Y]`・`double-click`・`down`・`up`・`drag X1 Y1 X2 Y2 [STEPS]`・`wheel N`・`hwheel N`・`key NAME[+NAME...]`・`key-down`・`key-up`・`type TEXT`・`sleep MS`・`tap X Y`・`double-tap X Y`・`touch-drag X1 Y1 X2 Y2 [STEPS]`・`touch-down ID X Y`・`touch-move ID X Y`・`touch-up ID`・`stop`。`aat` の各命令は 1 つか少数の `aat-input` の命令に対応する（SSH の 1 回ずつ、ControlMaster で速い）。
- 撮影: `keiland-shot OUT.png`（compositor が合成した画面、root）。aat は書かれた PNG の大きさを画面の大きさとして `aat-input start` に渡す。撮影に失敗した時は、その出力・終了の状態・user・`ls -l` を言う（T1-200 の「not found」の手がかり）。
- `aat-input start` の server は起こされた時の descriptor を持ったまま背景に残るので、aat は出力を file（`AAT_RUN_DIR/input-start.txt`）に向けて起こし、起こした側が返った後にその file を読む（SSH の出力に向けると session が終わらない、T1-200）。
- `check` の status は注入・`aat-input`・撮影の 3 つで決まる。session の log は有無を言うだけ（session がまだ無い image でも 0）。

## P1 への依頼（log）

窓の座標の helper のために、compositor の log に次があると良い（無くても `windows` は動くが、窓を app で探せない）:

- 窓の app と題名: 例 `ZWL TOPLEVEL surface=S app_id=ID title=T`（map と題名の変化の時）。
- 窓の今の位置と大きさの一覧を出す口（例 `keiland-shot --windows`）。

## 自己試験

- host（実装の担当）: `sh plan/tools/aat/tests/run-host.sh` → `aat-host: PASS`。CLI・`aat-input` に送る命令・拒否・mark と wait-log・`windows`（client ごと）・転送（ssh を通ること）・シナリオの文書と suite の形・runner（補助の pass、hands の needs-person、記録と撮影）を、偽の `aat-input` と撮影で確かめる。
- QEMU（T1）: 撮影の口のある image（`plan/ws173/tests/config-amd64-aat.mk`、T1-200 の物でよい）を `plan/tools/titlebar/menu-guest.sh start IMAGE`（`GUEST_RUNTIME=build/ws070-run`）。この image に session は無いので log は `--log /tmp/zdesktop.log`。
  1. `aat --qemu check`（rc 0: `have /dev/input-inject`・`have /bin/aat-input`・`have /bin/keiland-shot`、log は有無だけ）、`aat --qemu run 'uname -a'`、`put`・`get` の往復、`mark`・`run 'echo hello >> /tmp/x.log'`・`wait-log hello --since m --log /tmp/x.log`。
  2. compositor と窓（`plan/ws173/tests/aat-p002.sh` の 1. と同じ: `aat --qemu run` で `/bin/wayland --testing --width=1280 --height=800 --glass > /tmp/zdesktop.log` と `seat-probe`）。
  3. `aat --qemu start`（数秒で返り `screen 1280x800`）、`aat --qemu shot a.png`（PNG 1280x800）、`click`・`drag`・`wheel`・`key`・`type`、`windows --log /tmp/zdesktop.log`、各段で `shot`、`stop`。合格: 各命令が 0 で終わり、PNG に操作の結果が見える。撮影が失敗したら、aat の出す行（状態・user・`ls -l`）をそのまま返す。
  4. 全部の項目（`run-aat.sh`、ws173-p004）は session のある AAT の image（`config-amd64-aat.mk`）で。
