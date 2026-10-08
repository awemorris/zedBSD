<!-- awesome-plan project=zedbsd record=ws094-p009 -->

# ws094-p009: L3b 100 項目で L3 の数値目標に入れる

Status: uncleared（2026-10-08 Q1 の判定（sweep-beta2-rc2 §2.1）: ベータ3 の性能の Phase へ回す。ベータ2 の完了の条件から外す）（旧: uncleared（2026-09-30、QEMU の Venus。(a)(c) は目標に届かない。残りの大部分は Files の外（zdesktop の import と、QEMU の Venus の呼び出し 1 回ごとの約 10 ms）で、進めるには main の判断が要る。実機は未実施））
Disposition: normal
Parent: [WS094](../ws.md)
Queue: main（Q1）の依頼（2026-09-30、P4、worktree `.claude/worktrees/ws090-widgets`、branch `wt/ws090`、`git merge main -m WIP` の後）

## 範囲と受け入れ

p008 の基準値のうち目標を超えた (a) 起動から `DESKTOP ready` まで（2908 ms、目標 1500）と (c) click から選択の frame まで
（95 ms、目標 50）を直す。Q1 の指示の順: まず (a) の内訳（exec・configure・listing・thumbnail・最初の描画）を取り、大きい所から直す。
(c) は選択 1 回で全面を描き直しているかを確かめ、描き直しの範囲を絞る。配置は変えない（100 項目のうち 91 だけが出るのはユーザーの判断で
「今のまま」、2026-09-30）。jpg・gif の thumbnail の error=21 は原因の見当だけ書く。

受け入れ: `perf100` の 3 回の中央値で (a) ≤ 1500 ms、(b) ≤ 2500 ms、(c) ≤ 50 ms、`SLOW-FRAME` 0。

## 内訳（計測）

`perf100` に内訳の行を足した（`perf100-steps.txt`）。Files の `ZFILES DESKTOP startup` は main() に入った時刻と各段階の ms、exec は
zdesktop の `ZWL DESKTOP start` から main() まで。調べるための一時的な log（present.c の各段、zdesktop の import の各段）は取った後に外した。

直す前（3 回、ms）:

| exec | fonts | window（接続・desktop の役割・configure） | present（Vulkan） | app（listing） | menus（context menu の service） | canvas | draw | shown |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 9〜13 | 13〜15 | 1114〜1125 | 857〜993 | 6 | 839〜861 | 14〜18 | 38〜40 | 112〜146 |

分かったこと:

- **Files 自身の CPU の仕事は小さい**: exec・fonts・listing・layout・最初の描画を合わせて約 80 ms。thumbnail は最初の frame の後に
  1 回に 1 枚ずつ作られていて（`THUMB` の行は `READY` の後）、ready を遅らせていない。
- **window 約 1100 ms**: zdesktop は Files を起こした後、自分の出力を開く（`ZWL MODE window switch_ms=635〜686`: pipeline・swapchain）。
  その間 Files の最初の roundtrip は待たされ、その後も zdesktop の 1 回の合成（`ZWL PERF compose frame_ms` 110〜150）ごとに 1 往復しか進まない。
- **present 約 870 ms**: Files の Vulkan の準備。instance 145、device 50、swapchain 309、pass〜descriptors 180、canvas 114、pipeline 60。
- **menus 約 850 ms**: context menu の service を探す 1 往復が、zdesktop が Files の swapchain の 3 枚の buffer を import し終わるまで待つ。
  import は 1 枚約 210 ms（vkCreateImage 10、メモリの import 20、view 10、layout の変更の submit と `vkQueueWaitIdle` 100、descriptor set 10、
  linear の set 20 など）。3 枚で約 630 ms と、その間の合成。
- **Venus の同期の呼び出しは 1 回ごとに約 10 ms**（10 ms の倍数に揃う）。guest の `nanosleep` の分解能は 1 ms（一時的な試験
  `build/ws094-tmp/sleepres` で確かめた）で、kernel の Venus の transport は IRQ で起きる（`src/drivers/gpu/venus/transport.c` の waitq）。
  host の QEMU の virtio-gpu の fence の見回り（10 ms ごと）が原因という見当だが、確かめていない。(a)(c) の残りはほとんどこれで、QEMU に固有の可能性が高い。

## 変えたもの

| file | 変更 |
| --- | --- |
| `userland/desktop/files/main.c` | 起動の各段階の時刻（`struct main_startup`）と、最初の frame の後の `DESKTOP startup` の行。desktop は Vulkan の instance を window より前に作る（zdesktop が出力を開いている間に進める）。desktop の context menu の service は最初の frame の後で開く（探す 1 往復が import を待つため）。canvas を作り直したら desktop の保持した frame を捨てる。`DESKTOP select-frame` の行に内訳（draw・copy・acquire・submit・queue・wait） |
| `userland/desktop/files/present.c`・`window.h` | `fm_present_instance`（instance だけ先に作る。`fm_present_open` は作られていなければ作る）。frame の record と submit の時間 `submit_ms` |
| `userland/desktop/files/ui-desktop.c`・`files.h`・`desktop-layout.c` | **変わった cell だけの描き直し**: 各 cell を描いたときの記録（場所・選択・cut・thumbnail）を持ち、同じ listing・大きさで、項目の上に何も無い（band・drag・drop・message・進行・質問・名前の欄が無い）ときは、記録の変わった cell だけを消して cell の clip の中で描き直す。項目が別の cell へ動いたときや、上に何かがあるときは全体を描く。選択された名前の pill を cell の中に収めた（長い名前で左右に 1 px 出ていた） |
| `userland/desktop/wayland/desktop.c` | `ZWL DESKTOP drawn at_ms`（desktop の program が起動してから最初に zdesktop がその image を描いた時刻）。Files の ready ではなく画面に出た時刻を測る (a') に使う |
| `plan/ws094/tests/files-desktop-guest.sh` | `perf100` に (a') `drawn_ms` と内訳の行（`perf100-steps.txt`） |
| `plan/ws094/tests/host-desktop.c` | 部分の描き直しの試験: 選択の変化の後の frame が、描いていない所（隅の印）を残し、同じ状態を全体で描いた絵と 1 画素も違わない（1 つ選ぶ、長い名前を Ctrl で足す、全部外す） |

試して戻したもの: desktop の swapchain を 2 枚にする（import を 1 枚減らす）。surface の minImageCount が 3 で、3 枚のまま変わらなかった。

## 確認

| 確認 | 命令 | 結果 |
| --- | --- | --- |
| build | `make -j16 ZEDBSD_CONFIG=plan/ws035/tests/config-amd64-zdesktop.mk BUILD=build/amd64 build/amd64/bin/files build/amd64/bin/wayland build/amd64/dynamic/libkeiland.so` | rc 0、warning 0 |
| style | `plan/tools/style-check.py`（ui-desktop.c・files.h・main.c・present.c・window.h・desktop-layout.c・wayland/desktop.c・host-desktop.c）、`git diff --check` | 0 件（main.c の既存の 2 件を除く） |
| host | `plan/ws094/tests/host-desktop.sh`、`plan/tools/files/host-model.sh` | PASS、PASS |
| guest: 計測 | `BIN=build/amd64 files-desktop-guest.sh build/ws094-shots/p009 install perf100`（image は `build/ws081/demo-win-venus.img` の複写） | 下の表 |
| guest: 回帰 | `… install show watch input saved`、`… install show menu`、`… install show drag` | PASS、PASS、PASS |
| boot test | `OUTPUT=build/ws094-p009-boot plan/tools/boot-test.sh build/ws094-run/disk.img` | PASS（`build/ws094-p009-boot/login.png`） |

touch の手順（pen image が要る）は流していない。

## 結果（QEMU の Venus、3 回の中央値）

| 値 | p008 | 直す前（p009 で再測） | 直した後 | 目標 |
| --- | --- | --- | --- | --- |
| (a) start → `DESKTOP ready` | 2908 | 2868 | **1954**（超過） | 1500 |
| (a') start → zdesktop が最初に描く | — | 3016 | **2973** | （参考） |
| (b) 追加 → 表示 | 1094 | 843 | 1344（以内。2 秒ごとの監視の位相で揺れる） | 2500 |
| (c) click → 選択の frame | 95 | 131 | **90**（超過） | 50 |
| `SLOW-FRAME` | 0 | 0 | 0 | 0 |

- (a) の短縮（約 900 ms）の大部分は、context menu の service を最初の frame の後に回したこと。**画面に出る時刻 (a') はほとんど変わらない**
  （3016 → 2973）: Files の frame は zdesktop が buffer を import し終わるまで画面に出ないため。instance を先に作る分は約 100 ms。
- (c) の Files の描画は約 38 ms → 1〜2 ms になった（`select-frame … draw=1`）。残りの約 90 ms は Vulkan の呼び出し（submit 32〜51、
  vkQueuePresentKHR 4〜51、fence の wait 1〜11）で、Venus の 1 回約 10 ms による。
- 画面（`build/ws094-shots/p009/perf100.png`）: 3 回の click の後、folder-3 だけが選ばれ、先に選んだ folder-1・folder-2 に跡は残っていない。

## jpg・gif の thumbnail の error=21（見当。直すのは別の Phase）

error 21 は `EOPNOTSUPP`（= `ENOTSUP`、`include/uapi/errno.h`）。Files の `fm_image_load`（`userland/desktop/files/thumb.c`）は
binary PPM・PGM と PNG しか decode せず、他の形式は `ENOTSUP` を返す。jpg・gif は形式として読まれていない（壊れているのではない）。
imageview（WS093 など）の jpg・gif の読み手を Files の thumbnail から使えるようにするのが直し方の候補。

## 残り（main の判断が要る）

1. **zdesktop の import を速くする**（WS035 の `userland/desktop/wayland/import.c`、この Phase の範囲外）: `import_layout` の submit と
   `vkQueueWaitIdle`（1 枚 100 ms）を、次の合成の command buffer の barrier にまとめれば、3 枚で約 300 ms 減る見込み。(a) と (a') の両方に効く。
2. **QEMU の Venus の 10 ms**: 原因（host の QEMU の fence の見回りという見当）を確かめ、QEMU か virglrenderer の設定で縮められるか調べる。
   (a)(c) の残りの大部分で、実機（L5、5330 の i915）では出ない可能性が高い。QEMU の目標を実機の目標と分けるかどうかも判断が要る。
3. Files の frame ごとの Vulkan の呼び出しを減らす（image ごとの command buffer を前もって記録し、reset・begin・end を毎回しない）。
   (c) の submit の 30〜50 ms のうち一部が減る見込み。Files の範囲なので、1・2 の後の Phase にできる。

再開の条件: 上の 1〜3 のどれを行うか、また QEMU の目標をどう扱うかを main が決めたとき。
