<!-- awesome-plan project=zedbsd record=ws141-p003 -->

# ws141-p003: display（firmware出力先の特定 → Linux順の再初期化 → 初回scanout → flip/合成/統合）

Status: in-progress（i11でbuffer/合成/登録/起動診断を実装・host/build確認済み。実機受け入れは未達）
Disposition: normal
Parent: [WS141](../ws.md)
Queue: none
依存: [p002](../phase002/phase.md)（骨格・段の印・P0。p002 の QEMU の回帰と実機の P0 の写真が先にあると安全）
実行者: 独立Codexセッション（旧P2 generation13の実装を引き継ぐ）

## 現行の設計変更（2026-10-09、i08）

ユーザーの「Linuxドライバと寸分違わず同じ手順…VC4の初期化と、scanoutの開始まで」と追加回答「Linuxと同じ再初期化へ変更する」で、下の旧N1コピー→N2通知→P4後回しは今回の起動の現行手順ではなくなった。理由は固定Linuxの起動経路がfirmwareのdisplay終了を通知してからHVS/HDMIを初期化し、初回commitでchannel・PHY・pixelvalveを新たに構成するため。途中の画面消失は承認済み。旧コード/結果は履歴として保持。

初期化の前にboot framebufferを表示する出力とfirmware modeを読む。表示終了通知→HVSの初期化→HDMI clock/reset→PVの準備→初回commit/scanoutのhardware操作を固定sourceの実処理順へ対応させる。mode/portの選択範囲は以前のfirmware mode/portのみ。新規mode選択・DDC/EDID追加・他portの点灯は含めない。準備の検証に失敗したら通知/resetを始めない。scanout開始はcurrent listの一致とPVのframe境界を確認して判定し、単なるenable bitで成功にしない。

このattemptでhost/buildまで確認できても、Q1経由T1のQEMU回帰と実機写真/scanout観測はwhole Phaseの関門として残る。LinuxとのOS glueの差、非対応mode、未確認のfirmware framebuffer寿命を明示する。HAL APIが必要と分かった場合の具体差分事前承認は保持。詳細と限定mailbox修正の判断は[実行記録i08](../execution-20261009.md#i08の承認設計変更2026-10-09)。

## 旧設計の履歴

## 範囲（[design](../rpi4-gpu-design.md) §3.1・§10 の p003）

段ごとに実機の写真で確かめながら進める。各段は `rpi4gpu.stop=<段>` で止められ、危ない書き込みの前に begin の行と 3 秒の待ちを出す（p002 の helper）。register の名前は `plan/ws141/temp/rename/rename-map.tsv` の独自の名前で書く（GPL の名前を写さない）。

1. **N0**（読むだけ）: mailbox の get の tag だけで firmware の framebuffer（物理・pitch・幅・高さ）と clock（core・HDMI の 2 つ）。HDMI の clock が 0 なら HVS より先は読まず「display 無し」で抜ける。0 でなければ HVS の全体の enable、出力の切り替え、channel の enable・幅・高さ・状態・次と今の display list の位置、display list の解読（plane の数・format・位置・大きさ・pointer・pitch・end）、pv2・pv4 の enable と timing を読み、80 桁の複数の行に出す。
2. **N1**（画面を消さない引き継ぎ）: firmware の display list の word をそのまま写した list を、firmware の list と filter の係数を避けた領域に書き、読み返して一致を確かめ、channel の「次の list」に入れる。「今の list」が自分の位置になるのを時限付きで poll。
3. **N2**（N1 の直後）: firmware に display の終了を通知（mailbox、判断の項目 17 で値は事実として使う）。前後で HVS・pv2/4・display list・clock・framebuffer の memory と mailbox の答えを読み比べる。変わったら止まって記録する（設計を見直す条件、判断の項目 16）。
4. **P1**: pixelvalve の vblank と HVS の underrun の割り込み（p002 で登録した masked の handler を本物にして unmask）。1 秒あたりの vblank の数。
5. **P2**: 同期の page flip（driver が持つ 1 GiB より下の連続の 2 buffer、cache の clean、display list の切り替え、vblank で完了）。
6. **P3**: plane の合成（console の plane の上に CPU で埋めた plane、console の領域の外、背景の fill）。core clock の underrun の対処（判断の項目 15）。
7. **P5**: resident display（`drv_gpu_display_ops`）の統合と display の device の登録（`drv_gpu_register`、display の役、companion は V3D の device）。cap の bit は gpu.c の検査に従う（design §4）。

範囲の外: P4（HDMI の mode set、H5〜H10）、EDID（判断の項目 14、`rpi4-firmware.c` の拡張が要るときは最小で、既存の呼び手の挙動を変えない）、V3D（p004）、`include/hal/hal.h`（判断の項目 16 で要るなら差分を plan に置いて止める）。

## 受け入れ

- 各段の build（rpi4、warning 0）と、変えた所の host の試験（display list の組み立て・解読、timing の読み取りの計算）。
- QEMU（Q1 経由で T1）: raspi4b で boot が壊れない（N0 は firmware の revision で emulator と判定し、HVS の register を読まずに抜ける見込み、未観測。QEMU の revision が build の時刻に見える値なら HVS を読んで bus の error になりうるので、この回帰は実機の前に必須）。
- 実機（ユーザー、判断の項目 6）: 段ごとの写真で印と画面（N1・N2 で画面が変わらない、P1 の vblank の数が 60±1、P2 の flip、P3 の重なり）。
- `rename-map.tsv` の旧名で driver の source に一致 0。

## 実機の手順

（p003 の実装のときに書く。p002 と同じく HDMI0 の画面の写真。serial は画面が消える段だけ。）

## 結果（2026-10-04、P2 generation13、途中でラップアップ）

- N0 を実装（hardware には書かない）: `src/drivers/gpu/bcm2711/readout.c`（N0）、`list.c`（display list の解読、host で試験できる純粋な関数）、`firmware.c`（`clock.c` を改名し mailbox の get の汎用の口 `bcm2711_firmware_get` と `bcm2711_clock_hz` を追加）。attach の口に firmware の画面（boot の handoff の物理・大きさ・幅・高さ・pitch）を渡す形に変えた（`drv_bcm2711_gpu_attach(fdt_phys, screen)`、`src/kern/platform/rpi4.c` が handoff から埋める）。
- N0 の順: firmware の revision を mailbox で読み、build の時刻（0x40000000 以上）でなければ emulator として register を読まずに抜ける（QEMU の raspi4b には compositor が無く、その register の読みは bus の error になりうるため。QEMU の binary の未実装の領域の名前に hvs が無いことを strings で確かめた。revision の値は QEMU で未観測）→ mailbox の framebuffer（幅・高さ・depth・order・pitch）と handoff の物理 → HDMI の state machine の clock（13）が 0 なら抜ける → HVS の全体の enable と HDMI0・HDMI1 の channel の選択 → 3 channel の enable・mode・大きさ・次と今の list → firmware の出力先の port（HDMI0 を優先）の今の list の解読（plane ごとに format・order・位置・大きさ・pointer・pitch）→ 最初の plane が framebuffer を 1:1 で指すかの判定 → pv2・pv4 の enable・video・active の大きさ。
- build: rpi4（driver y）exit 0・warning 0、rpi4 の driver n exit 0・warning 0（amd64 は rpi4.c を build しないので影響なし、未再試験）。host の試験: `plan/ws141/tests/stage-host-test.sh` が stage と list の 2 つを流し両方 PASS。改名の旧名 630 で driver に一致 0。`git diff --check` 0。
- **再開点**: (1) Q1 経由で T1 に QEMU の回帰（raspi4b の boot-test、login prompt。N0 は emulator の判定で register を読まずに抜けるはず。PNG に行が写らないので、印を見るなら serial の対話か boot の後の dmesg を SSH で読む道を T1 と相談）。(2) 実機の写真（ユーザー）で P0・N0 の行と期待値を照合（N0 の行は 15 行前後で、25 行の console から P0 の行が流れる。必要なら `rpi4gpu.stop=N1` で止めて写真）。(3) 次の段 N1（firmware の list を写した自前の list、「次の list」の切り替え、今の list の一致の poll）。N1 の前に実機の N0 の結果（firmware の list の位置・word の範囲・plane の数）が要る。
- 未実施: QEMU の回帰（N0 の版）、実機。

## 独立Codexセッションの再開確認（2026-10-09）

ユーザーがWS141を担当へ割当。開始tree a05865278のrpi4 kernelをdriver y/nでbuildし、両方exit 0・warning/error 0、stage/list host試験PASS。source修正は無し。詳細は[実行記録](../execution-20261009.md)。ユーザー回答「実機確認は後で行う」により実機条件は未達のまま保持。whole Phaseのclearanceは行っていない。

## N1の準備処理（2026-10-09、i03）

ユーザーの継続指示により、実機観測と独立な配置計算とraw wordコピーだけを実装。list.cの`bcm2711_list_copy_prepare`はsnapshotと予約範囲から終端込みの連続領域を選び、decoded summaryを再構成せず全wordを保持する。予約範囲には全channelのcurrent/next list、filter、firmware専有範囲を含める責務を呼び手に明記。起動経路での呼び出し・hardware書き込みは無し。

list-copy-host-test.cで順不同/重複予約、filter回避、SRAM枯渇とexact fit、9 planeとscaling/contextの完全一致、snapshot不変、失敗時のimage不変を確認しPASS。rpi4 y/n build warning/error 0、全文C review・補助style-check total 0。詳細は[実行記録 i03/i04の結果](../execution-20261009.md#i03i04の結果2026-10-09)。この部分attemptのみcleared、whole Phaseはin-progress。次は実機N0観測を元にsnapshotと全予約範囲の取得を統合し、再検証後のwrite/readback・次listの切り替え・時限付きpollを進める。p004の独立したsoftware準備も[WS](../ws.md)へ投影済み。N1の実機条件は保持。


## i08の保存結果と再開条件（2026-10-09）

- 部分範囲: bootで表示されている既存progressive RGB8 mode/portだけを選び、Linux順R0の初期化から初回scanoutまで実装・照合。新mode/他port/EDID/DDC・V3Dは含まない。
- 現行のsoftware確認条件: 通知前にmode・buffer・reg span・serviced IRQ依存を検証し、唯一のboot出力を選ぶ。固定hardware値/順序、両portのlane/FIFO/timing、失敗時の後続停止をhostで確認、rpi4 y/nをwarning/error 0でbuild。初回完了にはIRQがcurrent list43を観測したvblankを要求。host2試験・既存4host・y/n buildはPASS。
- 未達: 実mailbox clientは値なしtagを拒否するため、現kernelではR0 op0 EINVALでMMIO write 0。WS048の3 pathの限定提案を作業コピーで確認したが実source未適用、適用判断のユーザー回答待ち。したがってi08はuncleared、以前のwhole Phaseの実機/flip/合成/統合も未達。build PASSをnative scanout成功とは扱わない。
- 旧「N1/N2で画面が変わらない」という受け入れ条件は今回の再初期化についてwithdrawn。新hardware条件は元のport/modeへ戻り、R0 ok（採用された新listのframe）、console framebufferの寿命/内容・画面・vblank/underrunを実機で確認すること。framebuffer geometryの一致だけではRAM寿命を証明しない。実機はユーザーが後で実施、QEMUはQ1/T1経由で後続回帰として残す。
- 再開は限定mailbox修正の承認/依存統合から。同じPhaseの新attemptに前回unclearedを保持して結果を記録し、最新mainとの統合検証へ進む。HALの契約拡張が必要なら具体差分を事前提示。exact commands/参照版/hash・skipped checks・Linux全driverとの相違は[実行記録i08](../execution-20261009.md#i08の結果と再開条件2026-10-09)。WSへの設計変更/受け入れへの影響は[WS記録](../ws.md#i08の設計変更と依存待ち2026-10-09)。


## i09: 依存判断の解決（2026-10-09）

ユーザー「mainにマージしてOKです。mailbox修正も承認します。」を取得。i08の未適用mailbox依存を提案のsource/header/host3 pathへ適用して解消し、実sourceのmailbox hostとdisplay host、rpi4 y/n build warning/error0を確認した。p003をin-progressへ戻し、最新mainとの統合検証へ進む。以前のi08 unclearedは保持。whole Phaseの実機/flip/合成/登録の受け入れはまだ未達。結果とcommandsは[実行記録i09](../execution-20261009.md#i09-mailbox実sourceの確認2026-10-09)、WSへの影響は[WS記録](../ws.md#i09-依存修正とmergeの承認2026-10-09)。


## i09の統合・部分clearance（2026-10-09）

承認済みmailbox修正とLinux順R0初期scanout成果を最新mainへ統合済み（dde7c1ba7）。統合版の実mailbox host/display hostとrpi4 y/n buildがPASS、warning/error0で、i09のsoftware/統合範囲をclearedにした。whole Phaseは実機/flip/合成/登録が未達のためin-progressのまま、closeしない。i08のuncleared履歴を保持し、実機の元の出力先・新list frame採用・buffer寿命/IRQとQ1/T1の回帰を再開条件に残す。commands/版/hash/未実施とWSへの投影は[統合記録](../execution-20261009.md#i09-mainへの統合結果2026-10-09)と[WS結果](../ws.md#i09の統合結果2026-10-09)。


## i10: 同期flip部品の部分範囲（2026-10-09）

継続指示によりP1/P2のvblank sequenceとcaller所有bufferの同期flip/console復帰を実装する。起動のflip・連続buffer allocator・P3合成・公開GPU登録は次の統合点に残し、whole Phaseの受け入れを減らさない。R0と同じport/modeを守り、inactive listだけへ書き、timeout時は実際に参照されうるbufferを保持、復帰のfresh frameを観測してから退役する。stateのIRQ/worker排他も部品に含める。[承認・条件・検証範囲](../execution-20261009.md#i10の選択実装範囲2026-10-09)。


## i10のsoftware確認・残件（2026-10-09）

同じR0 pipelineを使う実MMIOの同期flip/console復帰とIRQ状態を追加、短いhost3試験・rpi4 y/n buildがPASS。旧/新bufferの保持はadoptionで退役し、timeoutの不確かさはfresh console adoptionだけで解消する。allocator/登録/boot呼び出しは未接続。実機成功の証拠は無く、whole Phaseはin-progressのまま。softwareのexact commands/hash・限界・最新mainへの統合結果は[実行記録i10](../execution-20261009.md#i10の実装確認2026-10-09main統合前)。次の統合点は、保持maskを尊重する連続buffer ownerとdisplay ops/device登録。実機R0/IRQ/console RAM寿命・P1/P2のhardware条件はユーザーが後で確認する。


## i10の統合結果（2026-10-09）

実装8ca85c4a9、main統合181339820。統合版host3試験とdriver=y build PASS（warning/error0/checker3 PASS）、nはup-to-date。i10部分はcleared、whole Phaseはin-progress。未接続のbuffer owner/display登録・P3・実機R0/P1/P2/IRQ/console RAM寿命とQ1/T1回帰は保持。[evidence/再開点](../execution-20261009.md#i10のmain統合結果2026-10-09)。


## i11のsoftware結果と現行の実機手順（2026-10-09）

allocator/reference owner、2-plane/clock/adoption、ordinary copy presentのdisplay opsとboot登録を実装。起動のP1で1秒frame観測、P2でgreen/purple stripe付きprivate targetをflip、P3で右上半透明checkerを合成し、それぞれconsole復帰を確認してP5へ登録。各段のstop/pauseを保持。host4 PASS・driver y/n build warning/error0・全文C/補助style/改名確認済み。i11のsoftware部分だけcleared、実機/RAM寿命/Q1-T1回帰待ちでwhole Phaseはin-progress。[exact evidence/制限](../execution-20261009.md#i11-display所有合成登録のsoftware結果2026-10-09)。

実機ではR0 ok/current43→P1のcount（既存modeのrefreshと照合）→P2の2色と復帰→P3の透過/位置と復帰→P5 nodeを写真で確認する。stop=P1はR0まで、stop=P2はP1まで、stop=P3はP2と復帰まで、stop=P5はP3と復帰まで。実機はユーザーが後で実施と承認済み、今回写真/scanout成功の主張はない。p005はこのdisplay software出力へ接続し、Keiland renderingはp006後。[WS全体の変更](../ws.md#完成までの自走p006の実装範囲確定2026-10-09)。


## i11統合確認（2026-10-09）

実装d0141f6e2を専用worktreeでhost4/y-n build確認し、Q1の後続更新も保持してmain40ce86ac0へ統合。i11部分cleared、実機/console RAM寿命は未達のまま。i12でFDT name lookupを共通internal helperへ移すが、displayのlookup/translation契約は保つ。[結果](../execution-20261009.md#i11のmain統合とi12の再開2026-10-09)。

## 最終software監査（2026-10-10）

Linux順initial scanout/独自filter/flip/二node presentの最終sourceを[p007 audit](../p007-software-audit.md)で確認。対応hostとnamed rpi4 y/n build PASS、code修正と制限は[実行記録i15](../execution-20261009.md#i15-最終software監査と検証2026-10-10)。software結果で実機のwhole acceptanceをclearせず、Statusはin-progressを保持する。Master/共有投影はQ1。
