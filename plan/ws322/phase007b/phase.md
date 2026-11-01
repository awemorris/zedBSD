<!-- awesome-plan project=zedbsd record=ws322-p007b -->

# ws322-p007b JVT の残り 26 本

- 親: [WS322](../ws.md)。2026-11-02 03:45 Q1 が p007 の結果から作成。
- Status: in-progress（2026-11-02 03:45 P2 q1907-i01）
- 依存: p007。

## 目的

p007 の JVT（H.264 の適合の stream 101 本）で全部の絵が一致しなかった 26 本の原因を突き止めて直す。

- vkvideo-probe の parser・DPB の planner で止まった 13 本（Extended profile、JVC_C、TANDBERG ほか）: Extended profile のように VPU が扱わない物は、誤りとして正しく断る事を確かめる（受け入れ）。扱える物は probe の側を直す。
- 絵が違う 13 本（重み付き予測の CAWP1・CVWP1〜3_TOSHIBA_E、CACQP3_Sony_D、CABA3_TOSHIBA_E、FRExt1/3_Panasonic、Sony の "_3" 5 本（300 枚中 5 枚）、CVSE3_Sony_H、MV1_BRCM_D）: 原因（parameter set の書き戻し、重み付き予測の表、DPB、表示順、参照の作り）を切り分け、driver・libvulkan・probe の側で直す。

## 受け入れ

- 26 本それぞれが、全部の絵で一致するか、扱わない理由が明示されて誤りとして断られる。
- p007 の回帰（run-vf2.sh・run-vf2-play.sh）と host 試験（p004/run.sh）が前と同じ。
- 板を元に戻し cksum が一致する。
