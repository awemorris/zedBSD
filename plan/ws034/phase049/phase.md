<!-- awesome-plan project=zedbsd record=ws034p049 -->

# ws034-p049: 動かない host の試験を削除する

Phase ID: `ws034-p049`
Parent: [WS034](../ws.md)
Status: **cleared**（q353-i01、2026-09-24）
Phase disposition: normal
Queue: q353（q353-i01）
実行: メインセッション

## 経緯

2026-09-24 ユーザー指示「動かないテストは rewrite せず、削除しましょう。テストの保守のコストをいつまでも払う必要はありません」。

## 方法

1. `plan/ws*/tests` の runner（`run-*.sh`・`run-*.py`・`*-test.sh`・`*-test.py`）393 本を集めた。
2. **host で安全に走らせられるものだけ**を対象にした。QEMU・ゲスト・VFIO／GPU の passthrough（host の GDM を止めるものがある）・
   remote host・`sudo`・集約の `make check` を使うもの、名前に qemu・guest・boot 等を含むものは**走らせず残した**
   （[`not-swept.txt`](not-swept.txt)、82 本）。
3. 残り 307 本を 6 並列、1 本 300 秒で走らせた。出力 directory を引数に取る runner は、2 回目に新しい出力先を与えて再実行した
   （ws030 の 3 本は `build/` の下を要求するので、そこを与えた）。
4. 失敗を分けた:
   - **build できないもの**（header の置き場所の変更、`zedbsd/*.h` の旧名、消えた型・関数、production source からの関数の切り出しの失敗、
     link の失敗）→ 削除。
   - build でき、結果が失敗するもの → 1 本ずつ再実行して中身を見た。production source の構造を grep で確かめる古い gate、
     消えた platform（sun4u・x68k）の一覧、旧い build 規則の性質の検査は削除。**製品の不具合かもしれない 3 本は残した**（下）。
   - 引数に特定の入力（前の実行の証拠、firmware 等）を要するもの、local の Mesa の build が要るものは、動かないのではなく
     入力が無いだけなので残した。
5. 削除する runner が名前・glob で使う fixture（`.c`・`.h`・`.inc` 等）のうち、残る file から参照されないものも削除した。
   残る runner を壊す削除が無いことを、参照の検査と再実行（下）で確かめた。
6. 証拠の文書（`.md`・`.tsv`・結果の JSON 等）は履歴なので残した。各 WS の tests の README に削除の注記と一覧への link を足した。

## 結果

| | 本数 |
| --- | --- |
| 走らせた runner | 307 |
| 通った | 112（1 回目 107、出力先を与えた 2 回目 5） |
| 削除した runner | 185（一掃の 183 と、削除した runner を呼ぶ umbrella 2 本: `plan/ws018/tests/run-legacy-bootfs-removal-host-test.sh`、`plan/ws035/tests/run-p034-fixture-regression.sh`） |
| 削除した file の合計 | **430**（runner と、それだけが使う fixture。[`deleted-tests.txt`](deleted-tests.txt)） |
| 残した失敗 | 3（製品の不具合の疑い）＋入力が要るもの 5＋環境が要るもの 1 |

WS ごとの削除数: ws025 128、ws004 105、ws018 42、ws019 37、ws005 30、ws031 15、ws016 15、ws006 13、ws030 9、ws011 7、ws003 7、ws029 5、ws024 5、ws010 3、ws035 2、ws022 2、ws014 2、ws013 2、ws002 1。

### 残した失敗（製品の不具合の疑い → ws031-p049）

build でき、今の source を使って失敗する。試験が古いだけか製品の不具合かをまだ切り分けていないので、削除せず
[ws031-p049](../../ws031/ws.md) にした。

| runner | 失敗 |
| --- | --- |
| `plan/ws014/tests/run-venus-edid-test.sh` | `mode.count == 3U` の assert（EDID の mode の列挙の数） |
| `plan/ws030/tests/run-libvulkan-job-race-test.sh` | `race_wait` の `status == 0` の assert。112 秒で止まる |
| `plan/ws030/tests/run-libvulkan-external-fence-test.sh` | `vulkan_sync_job_reserve`（`sync.c:574`）の確保 72 byte が LeakSanitizer で漏れ |

### 入力・環境が要るので残したもの

`plan/ws004/tests/run-multiple-nvme-concurrent.py`・`run-multiple-nvme-followup.py`（前の実行の証拠）、
`plan/ws025/tests/run-high-memory.py`・`run-memory-compat.py`（firmware と mode）、`plan/ws030/tests/run-amd64-device-map.py`（`build/` の下の出力先を与えると通る）、
`plan/ws031/tests/run-vk-gentool-test.sh`（`plan/ws031/mesa-refs` の Mesa の build）。

## 残した runner の再確認

削除の後、通っていた 112 本をもう一度走らせた: **112 本とも通る**。

1 回目の削除では、依存の探索が「削除する umbrella（p034 の fixture 回帰）が名前で呼ぶ runner」を、他から参照されないという理由で
fixture と同じく削除の対象にしてしまい、通っていた runner 20 本（ws014 の GPU framework、ws004 の NVMe 等）を消していた。
再確認で 127（file が無い）として見つけ、全部戻し、**runner は依存として削除しない**規則で数え直した（480 → 430）。

## 残り

- 走らせていない 82 本（QEMU・ゲスト・hardware 等）は確かめていない。QEMU を使う runner の一掃は別に行う必要がある
  （起動に時間がかかり、host の表示を止めるものを除く判断が要る）。
- 一掃で走らせた runner の一部は、tracked の解析 log（`plan/ws029/phase007/analyzer-*.log`、`plan/ws031/phase012/analyzer-gcc.log`）を
  上書きした。元に戻した。
