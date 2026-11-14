<!-- awesome-plan project=zedbsd record=ws217 -->

# WS217: 画面の回転（Settings の Display、縦長で Notes を使う）

<!-- awesome-plan-current:start -->
Status: incomplete（2026-10-28 ユーザーの回答で開き直した: p005（複数 head の回転）・p006（pen・touch と display の結び付け）・p009〜p012（scanout の 180°・Pi 4・game の session）が未着手で行き先が無いため。2026-10-27 の P3 の精査で一度 completed にした時の判断: Settings の回転と Notes の縦長の目的は p002a・p002b・p003・p004 で達成（T1-576・T1-592・T1-598・T1-611〜T1-616 PASS））。残り・制限: p009a は 2026-11-15 に main の上へ載せ直し中（agent/p5-ws217）、p009b（i915）・p009c（libvulkan）は branch `agent/p2-ws217` にだけある実装（試験は host と build だけ）、p010・p011・p005・p006・p012・p007・p008 は未着手、実機の確認は未実施
Resume point（2026-11-15 P5、q2110-i01）: 記録の最後（2026-10-29）から main を照らした。p009-1（Intel の PRM の確認）は 2026-10-13 に済んでいた（[phase009](phase009/phase.md) §1〜§4）ので表を直した。前提の WS309 は 2026-11-15 に completed、WS353 も main に入った。p009a（UAPI・核の検査）を main 44bb7cdaac の上に載せ直し（`ca9f3d491e`、[phase009a](phase009a/phase.md)）、host 試験・x86_64・rpi5・vf2 の kernel の build・QEMU の回帰を流した。p009b（i915、5330・SV7 の実機で通るまで main に入れない）と p009c（libvulkan）の載せ直しの見込みと手順は [phase009b](phase009b/phase.md) §5・[phase009c](phase009c/phase.md) §5。次: p009a の merge（Q1）→ p009c の載せ直しと QEMU の回帰 → p009b の載せ直しと 5330・SV7 の実機 → p011（Pi 4）→ p010・p005・p006・p012 → p007・p008。注: build system が変わった（`make CONFIGMK=config/config.<platform>.mk …`、`BUILD=build/<platform>`）。`plan/ws217/tests/config-amd64.mk`（AAT の image）と各 phase.md の `ZEDBSD_CONFIG=config/ci/…` の command は旧い形で、そのままでは動かない（使う時に今の形へ直す）。旧 Resume point（2026-10-29 P2、q1680-i01）: p009a・p009b・p009c を agent/p2-ws217 に実装（試験未実施）。旧 Resume point（2026-10-29 P5、q1666）: 残りの段は [design-v5.md](design-v5.md)（第 5 版 r2）。順は p009a → p009b・p009c・p011 → WS309 の後に p010・p005・p006・p012 → p007・p008、計 91 LW。
Primary Milestone: MG006
Related Milestones: なし
<!-- awesome-plan-current:end -->

## 原文（2026-10-13 ユーザーの UAT）

「・SettingsのDisplay設定に、画面回転を追加してほしいです。縦長でNotesを使いたいです。」

## 目的と完了の条件

1. Settings の Display に display ごとの回転（0°・90°・180°・270°）があり、選ぶとすぐ反映し、再起動後も保たれる。
2. 回転した画面で、窓・App Home・Wiseview・system bar・スクリーンキーボードの配置が新しい縦横に合う。
3. touch panel・ペン（tablet）・mouse・touchpad の座標が回転に合う（ペンで Notes に縦長で書ける）。
4. i915（5320・5330）で動く。Pi 4（vc4）・QEMU でも動くか、動かない所を記録する。
5. 描画の速さが大きく落ちない（plane の回転が使えればそれを使い、使えなければ compositor の側で回す）。
6. （2026-10-13 ユーザーの確認で追加）pen・touch と display の結び付けが Settings で選べ、「この画面に触って」でも決められ、再起動後も保たれる。結び付けの無い device は主の出力に写る（今の挙動）。

## Phase

設計の要点（p001 第 3 版 §0）: **180° は表示の hardware が回す**（UAPI に plane の回転の印 `GPU_DISPLAY_ROTATE_180`・`PRESENT_ROTATE_180`・`PLANE_ROTATE_180`、i915 は既存の `PLANE_CTL_ROTATE_180` の計算に `hw.rotation` を通す、libvulkan は display の `supportedTransforms`・surface の `transform`・swapchain の `preTransform` に ROTATE_180 を受ける、compositor は surface を transform つきで開き直す。Pi 4 は HVS の 2 つの反転 bit）。**90°・270° は compositor の turn pass**（論理の大きさの turn.image に描き 1 pass で swapchain へ。Intel は全部。Pi 4 は WS141 p015 の後、それまで rpi4 の build で止めて無効表示）。**能力の policy**: hardware が 180 を回せない display（Venus、KMS、古い driver）は回転を一切出さず、display ごとの選べる角度と理由を protocol の `rotations` で Settings へ（無効表示）。試験のためだけに `--rotate-policy=any`。物理の mode は `compose->mode_width/height`、論理は `server->width/height`（書くのは 1 つの関数の 2 行、回転は `compose_display` で config から当て、open の全経路で論理の大きさを前後で比べる）。相対の入力は変えず、touch screen と絶対の pointer は常に direct（物理→逆回転→論理）、pen は `INPUT_PROP_DIRECT` で分け、机の上の pen tablet は論理に直に。head に結んだ指は head の矩形で clamp（edge・corner・OSK の gesture は anchor だけ）、mirror は fit の逆写像で anchor へ。pen・touch は identity（bus・vendor・product・serial、空白は `_`）で display に結ぶ（Settings の card／「Touch to assign」、pen は down だけ）。protocol は manager version 34 の `set_rotation`・`bind_touch`・`listen_touch` と event `rotation`・`rotations`・`touch`（wire は p004 で全部）、libkeiland は getter（ABI 不変）、wl_output は transform normal のまま mode を論理に（案 E）、保存は `displays.conf` の `rotate=KEY 90`・`touch=IDENTITY KEY`。回転した出力（180° の scanout も）では game mode を合成に落とし、cursor の plane と overlay を使わない。

| Phase | 内容 | Status | 依存 |
| --- | --- | --- | --- |
| [p001](phase001/phase.md) | 設計: scanout の 180° と compositor の 90/270、能力の policy と告知、論理と物理、入力の座標の変換、game の session の回転の停止、pen・touch と display の結び付け、Settings と保存の形、段の分け方、試験 | planning（第 4 版 2026-10-13 P4。その後 p002a〜p004 が第 4 版で実装され、残りの段の設計は [design-v5.md](design-v5.md)（第 5 版 r2、2026-10-29）が最新。2026-11-15 P5: この行の Status は 2026-10-13 のままで、どの状態にするかは Q1 の判定） | — |
| [p002a](phase002a/phase.md) | compositor の中核、turn 無し: `rotate.c`（写像・plan・mask と why・`kwl_rotate_steps`）、`mode_*`・`size_*`・`kwl_output_size_set()`、`rotation_selected`／`rotation`、config の `rotate=`（`compose_display` と open の全経路、**open の前後の（論理の大きさ, rotation）の組の比較と手順**）、wl_output の論理（案 E）、scanout の fact と `KWL_SCANOUT_ROTATED`、cursor の plane の停止、`--rotate=N`、`KWL OUTPUT rotate`、`SHOT`/`DISPLAYS` の `rotation=`、`compose_target_*` の helper（0° の値）と p001 §1.8 の置き換え。0 以外は UNSUPPORTED | **cleared**（T1-576 の (A)(B)(C) PASS、Q1 判定 2026-10-13。実装 agent/p4 `b58ff4b06`） | p001、WS213 p004 の scoped な出力（main 済み） |
| [p002b](phase002b/phase.md) | turn: turn.image・2 つの pass・pipeline（`turn.vert`＋既存の `quad.frag`、NEAREST）・CLEAR/LOAD の選び方、damage（turn.image の age と swapchain の物理の矩形）、scene/backdrop/layers/glass の論理の大きさ、`kwl_output_select_rotation`／`kwl_output_apply_rotation`（review-4 S12、先に作る・park と cursor の hold・同じ物理の点の pointer・大きさが同じなら作り直さない・保存）、open の turn の資源の失敗 → 0、試験の image だけの `--rotate-policy` と shot.c の `POLICY`・`ROTATE`・`SHOT partial`、GPU の vendor で既定の turn（Pi 4 は無効） | cleared（2026-10-14、T1-583・T1-592） | p002a |
| [p009-1](phase009/phase.md) | Intel の公開の PRM で linear の 180 を確かめる段（引用と出典を p009 の phase.md に） | 済み（2026-10-13 P2、phase009 §1〜§4。止める理由は見つからなかった。2026-11-15 P5 が表を直した。cleared の判定は Q1） | p001 |
| [p009](phase009/phase.md) | scanout の 180°（kernel・libvulkan）。design-v5 A2 で p009a・p009b・p009c に分けた: UAPI の 3 つの定数と ops の印 `DRV_GPU_DISPLAY_ROTATE_180`、核の検査、i915（能力の印、**worker の present の item の `rotation`** → flip → `hw.rotation`）、libvulkan（display の `supportedTransforms` と surface の transform だけ、capabilities と swapchain は不変、present/commit の flag）、vkdemo の `open_on` の transform | p009a・p009b・p009c に分けた（下の 3 行） | p009-1 |
| [p009a](phase009a/phase.md) | UAPI の 3 つの定数・ops の印・核の検査（純粋な関数＋host 試験 34 項）と docs/reference の写し（design-v5 A2） | cleared（2026-11-15 Q1、024e3bb57b を main に merge 557ea0ffbf。host 34 項・ws217 の host 試験 7 本・3 機種の build warning 0・QEMU の qemu-render-wait（rotate 90/0）と boot-test PASS） | p009-1 |
| [p009b](phase009b/phase.md) | i915 xd（A1・A3〜A5）と xd9（A6）、host 試験（A8）、5330・SV7 の回帰 | in-progress（実装は agent/p2-ws217 の `8d51619d93` だけ、試験は host と build だけ。5330・SV7 の実機で通るまで main に入れない。載せ直しの見込みは §5） | p009a |
| [p009c](phase009c/phase.md) | libvulkan・vkdemo（第 4 版 §5.3、A8） | in-progress（実装は agent/p2-ws217 の `fb1b270d27` だけ、試験は build だけ。載せ直しの見込みは §5。WS309 は completed なので Q217-2 の順は待つ物が無い） | p009a |
| p010 | compositor の scanout の 180° と能力の policy: `scanout_rotation`、`hw180`（display の property）、surface の transform、REOPEN の経路（heads も閉じる）、`rotations` の mask と why（`WHY_RESOURCE`）、保存の角度を当てられない時の 1 つの規則 | 案 | p002b、p009 |
| [p003](phase003/phase.md) | 入力（anchor）: `struct kwl_cursor_geometry`、`kwl_output_absolute()`、touch・tablet・受け箱の置き換え、ROTATED の手順の geometry と同じ物理の点、direct の分類（touch screen と絶対の pointer は常に、pen は property）、傾き、kernel の hid-input の property の規則（BUG-267 の fixture）、注入の pen の DIRECT と `PEN_POINTER`（D12）、Linux・FreeBSD の backend の `EVIOCGPROP`（D11） | cleared（2026-10-14、T1-592・598・599） | p002b（p010 と並行可） |
| [p004](phase004/phase.md) | protocol・libkeiland・Settings の回転: version 34 の wire の全部（requests 7・events 6、`rotation` は選択の値、`why` の 4 bit）と KL_VERSION の次の API の全部、compositor は `set_rotation`・`rotation`・`rotations` を実装し `bind_touch`・`listen_touch` は UNSUPPORTED、`page-display.c` の 4 つの絵と理由の文、shown でない display の論理の snapshot、`keiland-desktop` の policy の file | cleared（2026-10-14、T1-611・613・614・616） | p002b、p003 |
| p005 | head の回転: `head->mode_*`、head の turn と scanout の 180、extended と mirror（元を scene の画像に、論理の fit、各 display が自分の向きで正立）、配置の再検査と `heads_windows_follow`、head の cancel・refit、head の open の前後の組 | 案 | p010、p004 |
| p011 | Pi 4: bcm2711 の能力の印と HVS の反転（180°、3 倍の buffer の実験）、`display-flip-host-test`、Pi 4 の GPU（vendor 0x14e4、`KWL_TURN_VENDOR_OFF`、p002b で実行時に判定）の既定の turn の無効（90/270 は WS141 p015 の後に外す） | 案（Pi の実機で受け入れ） | p009、p010、WS214 p003a/p003b の clear |
| p006 | pen・touch と display の結び付け: `kl_backend_input_uniq`（2 file）、identity、`touch=`、結び付けの引き直し、mirror の fit の逆写像、contact の矩形（head の clamp、gesture は anchor だけ）、`kwl_pointer_warp_to(slot)`、listen（pen は down だけ）、`bind_touch`・`listen_touch`・`touch` の中身、libkeiland の `kl_system_touches_*`、Settings の「Touch and pen」の card と「Touch to assign」 | 案（Q1 の判断で p006a/p006b に割ってよい） | p003、p005 |
| **p012** | **game の session の回転の停止**（ユーザーの決定「ハードウェアでできない回転を使っているときにゲームモードにすると、回転が無効になる」、p001 §4.12）: session の定義、`KWL_SCANOUT_UNROTATE`、外す・戻す（保存しない、配置も保存しない、他の窓は動かさない、90° の資源は保つ）、期限、session の間の `set_rotation`、`WHY_GAME`。rpi4・head・180 の scanout は対象外 | 案（**D18（video）の決定待ち**、game だけで実装してよい） | p002b、p003、p004 |
| p007 | 確かめ: T1 の scenario の全部、5330・5320（ユーザー: 180° の scanout、内蔵 panel とペン LCD の縦置きで Notes）、D5 の Notes の幅、Pi 4 の 180°（p011）と 90/270 の記録（WS141 p015 待ち） | 案 | p002〜p006、p009〜p012 |
| p008 | 規約の全文の見直し（p007 の直しの後） | 案 | p007 |

T1 の束（p001 §13）: A = p002b+p003（QEMU）、B = p009+p010（実機）、C = p004+p005（QEMU）、D = p011（Pi）、E = p006+p012（QEMU）、F = p007。

## ユーザーの決定（2026-10-13）

「ではあとは推奨でお願いします」 → 推奨で決定。D1 は数字ではなく倒す向きの絵で選ぶ（内部・file は時計回りの度数）、D2 hardware の回転はやらない（Future Work）、D3 は回転と一緒に pen・touch と display の結び付けを入れる（Settings で選ぶ＋「この画面に触って」で決める、自動の推測は後で。机の上の画面の無い pen tablet は INPUT_PROP_DIRECT で分けて回さない）、D4 すぐ反映・確認なし、D5 p006 で先に測る（第 1 版の段の番号。第 2 版以降は p007）、D6 回転した出力の game mode は合成に落とす、mirror は各 display が自分の回転に対して正立、回転した出力では cursor の plane を使わない（review-1 S3）。

2026-10-13 ユーザーの確認（原文）:「画面ごとに向きを持つ、ペン・タッチと画面の結び付ける」→ 回転は display ごと（mirror でも各 display が自分の向きを持つ、GNOME の共有の形は採らない）、pen・touch と display の結び付けをこの WS に含める（上の D3 の決定の確認）。

2026-10-13 Q1（第 2 版の残る問い D7〜D12）: ユーザーの「ではあとは推奨でお願いします」に沿って推奨を採る: D7 Pi 4 の回転の受け入れは WS141 p015 の後、D8 度数に統一、D9 login 画面は回らない（制限として記録、Future Work）、D10 案 E（wl_output は transform normal・mode は論理）、D11 Linux・FreeBSD の backend に EVIOCGPROP の読みを足す、D12 UAPI に `INPUT_INJECT_KIND_PEN_POINTER` を足し注入の pen を DIRECT に（HAL ではない、Q1 の判断で可、ユーザーに報告）。番号の割当: `KL_SYSTEM_SINCE_ROTATION` = 34・`KL_SYSTEM_MANAGER_VERSION` = 34（実装の時に main の最大を確かめ直す）、KL_VERSION は実装の merge の時に Q1 が次の空きを割り当てる（93 は WS210 と取り合い）。

2026-10-13 Q1: review-2 の判定「p002 はまだ始められない」（致命的 2 C3・C4、重大 3、中 10、軽微 15）。第 3 版で C3（turn pass の部分 frame は pass_load）・C4（回転は compose_display の中で当て、open の経路で論理の大きさが変われば resize の手順）と p002 に関わる M1〜M4 を直せば開始可。新しい問い S8（mirror の中の結び付けは各 display の回転で写す）と M8（WS141 p015 の前の Pi では回転を UNSUPPORTED）は推奨を採る（ユーザーに報告、変更可）。第 3 版の担当は次に空いた P。

## ユーザーの決定（2026-10-13、scanout の回転）

ユーザー（原文）:「はい、Intelのscanoutの回転機能を使ってください。KHR_displayでできそうですか？」「Venusでは回転できなくていいです。画面サイズを変えればいいだけなので。この場合は回転機能を無効表示にします。」→ Q1 が Linux i915 v6.17 の `skl_universal_plane.c:2959-2964`（display version 13 以降は plane の回転が 0°・180° だけ、12 以前は 90°・270° も、90/270 は tile の並びが条件）を示し、案を出した。ユーザー:「はい、回転はそれで実装してください。」→ 決定:
- 180°: Intel は全部、表示の hardware の回転（KHR_display の `VkDisplaySurfaceCreateInfoKHR.transform` と surface の `supportedTransforms`、swapchain の `preTransform`。libvulkan の IDENTITY 以外の拒否を外し、UAPI に plane の回転を足し、i915 で plane の回転を有効に）。Pi 4 は HVS の左右・上下の反転の組で 180°。
- 90°・270°: Intel は全部（5320 の Tiger Lake も）compositor の GPU の回転（第 2 版の turn pass）。Y-tiled の作業はしない（Future Work）。Pi 4 は WS141 p015 の後に compositor で、それまでは無効表示。
- Venus（QEMU）: 回転は無効表示（画面の大きさを変えればよい）。
- 第 3 版で review-2 の C3・C4・M1〜M4 と合わせてこの形にする。担当は次に空いた P。

2026-10-13 Q1（第 3 版の残る問い D13〜D17）: ユーザーの「推奨でいい」の流れに沿って推奨を採り、ユーザーに報告（変更可）: D13 test image だけ `--rotate-policy=any`、D14 hw180 の無い display は回転を出さない（Linux・FreeBSD の build も無効表示、KMS の rotation は Future Work）、D15 Pi の HVS の反転はまず tree と実機、GPL の参照は分からなかった時だけ（その時はユーザーに）、D16 UAPI の 3 つの定数（90/270 の bit は作らない）、D17 180 の scanout の出力で game mode の直の scanout はしない（Future Work）。
2026-10-13 ユーザー（原文）:「D14～D17  推奨でいいです。」
2026-10-14 ユーザー（D14 の変更、Settings の回転のため）: Q1「D14 のままだと 5330 も含め Settings の回転は全部選べない表示になります」→ クリック「D14 を緩める」: Pi 以外では 0・90・180・270 を turn pass で出す（hw180 が入れば 180 は scanout）。QEMU・Linux・FreeBSD でも回る。→ 上の Q1 の採った推奨を確定（D13 は Q1 の推奨のまま）。
2026-10-13 ユーザー（原文）:「Game Modeでは回転不要です。むしろ、ハードウェアでできない回転を禁止します。」→ Q1 の読み（ユーザーに確認中）: game mode の窓を回して合成に落とす D6 はやめる。90°・270°（compositor の回転）の出力では game mode に入らない（直の scanout も合成もしない、窓は通常の窓のまま）。180°（hardware）の出力の game mode は D17 のとおり今は直の scanout をしない。
2026-10-13 ユーザー（原文）:「ハードウェアでできない回転を使っているときにゲームモードにすると、回転が無効になる、でOKです」→ 決定: 90°・270°（compositor の回転）の出力で game mode に入ると、その間は回転を無効にして 0° の物理の向きで game mode の直の scanout をする（turn pass を外す、park の中で切り替え、入力の写像も 0° に）。game mode を出たら元の回転に戻す。D6（回した出力の game mode を合成に落とす）は置き換え。180°（hardware）の出力の game mode は D17 のまま（今は直の scanout をしない、Future Work）。次の設計の版で反映。

2026-10-13 Q1: review-3（致命的 2 C5・C6、重大 2 S10・S11、中 8、軽微 12）。判定: p002 全体（p002b）はまだ不可、第 4 版で C5（open の比較を（論理の大きさ, rotation）の組に、rotation の変化で cancel・geometry・resync・tell）・C6（game mode で回転を外す決定を game の session と直の scanout に分け、回転は session の間だけ外し保存しない、新しい段 p012）と M11〜M13 を直せば開始可。p002a（turn 無し、0 以外 UNSUPPORTED）は第 3 版のまま開始可（open の比較を最初から組で）。p009 は最初の段（Intel の PRM の確認）だけ今開始可、実装は第 4 版で S10（i915 の 180 は present の item で運ぶ）・S11（surface の transform を scanout が当て、capabilities と swapchain は IDENTITY のまま）・M17 の後。ユーザーへの問い D18（video の fullscreen にも game mode の回転の外しを当てるか、推奨は game だけ）。第 4 版の担当は次に空いた P。

2026-10-13 ユーザー（原文）:「Video Playerは、Game Modeをやめます。…Game Modeは別途、Wayland拡張で要求があったときのみ使用することにします。Game Modeを利用したアプリは現段階では存在しない、という整理にします。全画面ではコンポジターを利用しましょう。」→ D18 は不要（video は game mode に入らない、WS223）。p012 の対象は game mode を Wayland の拡張で求めた client だけ。

2026-10-13 Q1: review-4（致命的 1 C7、重大 5 S12〜S16、中 9、軽微 12）。p002a は C7（open の前後の比較の before は compose が持つ「最後に開いた・回した状態」と比べる。switch は open の前に server->width を書くので入口で取ると RESIZED が立たない、回転 0 でも壊れる）と L49 を p002a の phase.md に入れてから（S16 の steps の mode の引数も推奨）。p002b は C7・S12・M21 の後、p009 の実装は M23・M24・M25・M27 を p009 の phase.md に書いた後、p010 は S13 の後、p012 は第 5 版で §4.12 を直してから（WS223 p001 の game mode の求めの決定にも依る）。Q1 の判断: p012 を p007 の後ろへ回す（完了の条件は外れない、ユーザーに報告）。暫定の「turn の出力では game mode に入らない」は安全。

2026-10-13 Q1（WS223 p001 の依頼、第 5 版で反映）: p012 の session の定義は `KWL_SCANOUT_CONTENT_GAME`（WS223 の推奨 (a) wp_content_type_v1 の game のまま）。p012 と §12.2 の回帰の項 7・9 は WS223 の p002（規則から video を外す）・p003（wltest に `--content-type=`）に依存。§4.12 の「video は D18」・§10 の D18・§12.2 の 9 の末尾「Videoplayer は reason=rotated のまま」は「video は content type で合成・log 無し」に直す。
