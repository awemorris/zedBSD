<!-- awesome-plan project=zedbsd record=ws035p069 -->

# ws035-p069: App Home の PoC（デスクトップをめくってアプリを起動する）

Phase ID: `ws035-p069`
Parent: [WS035](../ws.md)
Status: cleared（q469-i01、2026-09-26）
Phase disposition: normal
Queue: q469-i01
承認: 2026-09-26 ユーザー「下記のアプリケーションランチャーも取り組んでほしいです。まずはPoCでよいです。私が実機で起動したときに、mviewやzdesktop-terminalを起動できるようにしたいからです。」
設計: [app-home-design.md](../app-home-design.md)

## 範囲（PoC）

1. zwl --glass に App Home: 左上の zedBSD アイコンのクリックと左上端から右下への drag で開く。デスクトップ（窓と壁紙）が
   右下へ退き少し縮み、右端と下端に角と影が残る。下から明るい Home（ぼかした壁紙を明るく）が現れる。
2. アプリの格子（6 列、64〜72 px のアイコンと名前）。アプリの一覧は設定 file（名前・command・印）から。最低限 Model viewer と Terminal。
3. アイコンのクリックでアプリを起動（fork と exec、`WAYLAND_DISPLAY` を渡す）し、Home を閉じる。
4. 閉じる: アイコンの再クリック、残った端のクリック、Esc、右下から左上への drag。
5. 文字を打つと検索（上部中央に文字、格子を絞る）、Backspace で空・Esc で解除。Enter で先頭を起動。
6. ページング・並べ替え・起動のアニメーションの細部は範囲外（1 ページ）。

## 受け入れ

1. Venus の guest で Home を開き、画面（VNC）で退いたデスクトップの角と格子を確かめ、Terminal と Model viewer を起動して窓が出る。
2. 検索で絞り込める。
3. 実機（i915、capture）の zdesktop の scenario で Home が開き、アプリを起動できる（可能なら）。
4. p052・p059・p062 の回帰、build は warning 0、style-check 0。

## 結果（q469-i01、2026-09-26）

実装（`userland/base/zwl/home.c` が新しい file、coding-style の全文、style-check 0）:

- 層: App Home を desktop の層の下に描き、desktop の層（壁紙と窓）を右下へずらし 3 % 縮める（`glass_shape_draw` に層の移動と
  縮尺、`server->layer_*`）。開き切ると desktop の左上の角（26 px、pointer が近いと 40 px）と影が右下に残る。Home の背景は
  最初から明るい（ぼかした壁紙を白く、淡い青）、icon だけが fade in。システムバーは上に残り、launcher に青い輪、docked の題名は隠す。
- 開く: launcher の click（バーの左上）、左上の角（28 px）からの右下への drag（14 px 動いてから gesture、対角の移動 360 px で
  全開、指を離して 30 % 以上なら開く、未満なら戻る）。閉じる: launcher、残った角の click、Esc（検索が無いとき）、起動。
  開く 280 ms・閉じる 240 ms（cubic ease-out）。
- 格子: 6 列まで、cell 144x152、icon 72 px（角 18、色、名前の頭文字を 36 px、上半分に光沢、影）、名前 15 px。中央寄せ。
- 検索: 打った文字を上部中央の淡い pill に（検索欄は出さない）、名前・command・keywords の部分一致で絞り、結果は中央寄せ、
  先頭を選択（青い輪）。Enter で起動、矢印で選択、Backspace で消す、Esc で検索を消す。
- 起動: `fork` と `/bin/sh -c`、子は setsid・zwl の fd を閉じ、`XDG_RUNTIME_DIR` と `WAYLAND_DISPLAY` を socket から。
  終わった子は tick で `waitpid` する。
- アプリの一覧: `/etc/zdesktop/apps.conf`（`name|command|keywords|RRGGBB`）、無ければ内蔵（Terminal、Model viewer、
  Vulkan test、Shared memory）。
- atlas に 36 px（icon の文字）と 24 px（検索）の大きさを足した（1024x512）。key は `zwl_seat_key` の先で Home が取る。
- 実機で使う image: `plan/ws035/demo/build-demo-image.sh`（`build/zdesktop-demo/hdd-image.img`、zwl --glass を起動時に、
  git 外の font と壁紙を入れる、zwl が終われば起動し直す）。zwl の `--timeout` の上限を 1 時間から 1 日に。

確認:

1. Venus（QEMU）: `plan/ws035/tests/zdesktop-p069.sh` PASS（`build/ws035-p069/run2/`、最終のコードで `venus-p069/`）:
   launcher で Home（`home.png`）、Terminal の icon で terminal の窓、角からの drag の途中（`gesture.png`: 明るい Home が左上から
   現れ desktop が右下へ）、`mod` の検索で Model viewer だけ（`search.png`）、Enter で mview（`mview.png`）、Esc と角で閉じる。
2. i915 実機（5330、VFIO、capture）: `CAPTURE=zdesktop-home ZDESKTOP_APP=home` で 4 検査 PASS（`build/ws035-p069/hw1/`:
   Home、Terminal の icon で terminal、Model viewer の icon で mview の 3D の窓）。
3. 回帰（Venus）p068・p052・p059・p062・p063 PASS。build は warning 0。boot test PASS（demo image、`build/ws035-p069-boot/login.png`、
   QEMU は i915 が無いので zwl は起動し直しを繰り返すが login に届く）。
4. 未実施: demo image の実機（5330 のベアメタル、LCD、内蔵の keyboard と touchpad）での起動。LCD を撮る手段が無い。

範囲外・残り（design の残り）: ページング（1 ページだけ）、並べ替え、起動の animation（icon の拡大と窓の生成）、single-instance
（起動中のアプリの窓を前に出す）、通知 badge、Home 中の仮想デスクトップの切り替え、keyboard の Tab、マウスホイール、
右下から左上への drag で閉じる（角の click だけ）、desktop の層の glass が動いた先でなく画面の位置の壁紙をぼかして映す。
