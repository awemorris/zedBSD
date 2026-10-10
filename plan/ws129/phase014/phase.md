# ws129-p014: nightly CIのnasm依存補完

Parent: [../ws.md](../ws.md)
Status: cleared
Disposition: normal
Queue: [ci-nasm-20261010](../codex-ci-queue.md)
Approval: 2026-10-10 最新ユーザー「ghコマンドが使えるので、GitHubのCIビルドエラーを解決してください」。
Scope / Criteria: GitHubの直近失敗CIをghで確認し、原因のあるnightly dependencyを修正。YAMLとshell syntax、nasmと実package設定/assembly probeの整合を確認、diff review、WIP commitでmain/push前の具体的成果を作る。remote CI greenをまだ主張しない。
Prerequisite: read-only gh取得済み、main/base39ebb1b06 clean。WS129 p004のrelease workflowはnasm導入済み。
Finding: runs38047175690/c43a01797と38013320115/e8adcdf89のBuild amd64が、FFmpeg configureの「nasm not found or too old」でError1→job exit2。nightlyのapt packageにnasmが無い。libavcodec Makefileは--x86asmexe=nasm、x86 asmを使う既存設計。
Design: .github/workflows/ci.ymlのSet up dependenciesにnasmだけ追加。asm無効化やtoolchain更新はしない。release.yml既存のnasmを維持。
Standards: AGENTS/Guardrail/standards/automationの境界と既存YAML/shell規約。C変更無し。新test suite・全体build/回帰・QEMU無し。full changed-scope reviewをこのPhaseで行う。
Verification bounds: YAML parse、apt install list照合、抽出runのbash -n、host nasmの小さいassembly probe。関連package既存configured/buildの読取り確認。現mainのremote run38057778279は未修正で進行中、結果が得られた時だけ記録。
Integration: main merge/pushの具体的承認を最後に確認。共有master/Queue/cacheは変更せずQ1へ投影待ち。Photosのmain統合承認待ちとは別task。

## 2026-10-10 ci-nasm-i01 outcome

cleared（workflow修正/有限host確認/commit準備の限定scope）。変更はnightlyのapt listにnasm追加1行。[検証証拠](../tests/ci-nasm-20261010.md)。YAML読み込みとbash -n、nasm2.16.03のELF64/AVX2 assembly warning0、git diff --check PASS。既存cross libavcodec buildのconfig/logで同じnasm使用と成功をread-only照合。C/source/toolchain変更無し、全体image rebuild無し。

main/pushは明示承認待ちで未実施。現mainのremote run38057778279はtoolchain stepがin-progress、修正後CI結果ではない。remote CI greenの条件をこの有限Phaseで達成したとは主張しない。承認された場合は規定push前チェック→統合/push→修正SHAのCI確認へ続く。WS全体のrelease acceptanceは維持。
