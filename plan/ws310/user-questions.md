<!-- awesome-plan project=zedbsd record=ws310-user-questions -->

# WS310 ユーザーへの問い

- WS: [WS310](ws.md) — ACPI の `_OSC` を呼び、システム全体の挙動を厳重に確かめる。状態: planning（p001 の設計を終えた、[design.md](design.md)）。
- 決まっていない事: 確かめの後に既定で呼ぶか、5330 で platform の `_OSC` が Thunderbolt の設定と CMOS に触れる時の扱い、CPU に何を申告するか、PCIe の制御の範囲、耐久の試験、仕様の入手。
- 用語:
  - **`_OSC`**（Operating System Capabilities）: OS が firmware の ACPI の method を呼び、「OS はこれができる（support）」「この機能は OS が扱う（control）」と伝える仕組み。firmware はそれを見て、自分で扱っていた処理を止めたり、隠していた情報（表）を出したりする。
  - **query と commit**: 1 回目は「これを求めたら何が得られるか」を聞くだけ（query）、2 回目で確定する（commit）。仕様では query は firmware の状態を変えない。
  - **platform-wide・processor・PCIe の `_OSC`**: 機械全体・CPU ごと・PCIe の根（host bridge）ごとの 3 種。
  - **`_CPC`**: CPU の性能の範囲（最高・標準・最低）を firmware が出す表。5330 では WS333 が直に読む CPU の register と同じ物を指す。
  - **CM（connection manager）**: Thunderbolt・USB4 の接続を管理する主体。firmware が行う（FW CM）か OS が行う（SW CM）か。zedBSD は SW CM を持たない。
  - **CMOS**: 電源を切っても残る小さな設定の記憶。BIOS が次の起動で読む事がある。

## Q310-1 確かめが通った後、既定で `_OSC` を呼ぶか

- 背景: 実装の間は既定で呼ばない（起動の語 `acpi.osc=on` の時だけ呼ぶ）。5330 の前後の比べ（design §6.3）が通った後に既定を変えるか。
- 選択肢:
  - A（推し）: 5330 の受け入れ（design §7 の 1〜3）が通ったら既定を `on` にする。`acpi.osc=off` で戻せる。
  - B: 既定は `off` のまま、使う人が `acpi.osc=on` を書く。
  - C: 5330 だけ既定 `on`（機械の判別が要る）。
- 推しの理由: 2026-10-29 の決定は「最終的には呼ぶ」。firmware は Windows の上と見ており、`_OSC` を呼ばない OS は firmware の試された道の外。戻しの語がある。

## Q310-2 5330 で platform の `_OSC` が Thunderbolt の CM の mode と CMOS に触れる時

- 背景: 5330 の firmware は platform の `_OSC` の commit で、OS が USB4 を扱わないと申告すると Thunderbolt の CM を firmware の mode に揃え（違っていれば Thunderbolt の controller に切り替えを送る）、設定によっては CMOS の 1 byte（index 0xF2）に書く（design §3.1 の 3）。CMOS は残るので、同じ 5330 の Debian（SW CM を使う見込み）と交互に起動すると互いに書き合う。実際に起きるかは firmware の値（`U4SE`・`U4CM`）次第で、p003 で読むまで分からない。
- 選択肢:
  - A（推し）: p003 で値を読み、切り替えも CMOS の書きも起きないと分かったら platform の `_OSC` を呼ぶ。起きると分かったら、その時に改めて聞く（それまで 5330 の試験は `acpi.osc=pcie,cpu` まで）。
  - B: 起きても呼ぶ（Debian と交互の起動で問題が無い事を p006 で確かめる）。
  - C: platform の `_OSC` は呼ばない（`_CPC` は出ない。目標の一部を諦める）。
- 推しの理由: 電源を切っても残る変更は戻しの語で戻らない。値を読めば判断の材料が揃う。

## Q310-3 CPU（processor）の `_OSC` に何を申告するか

- 背景: CPU の `_OSC` には「C-state を MWAIT で行う」「P-state を `_PSS` で行う」などの bit がある（Intel の文書、未入手）。zedBSD は idle を HAL が CPUID で、P-state を WS333 が MSR で直に行い、firmware の `_CST`・`_PSS`・`_PPC` を使わない。5330 は bit を申告すると `_PSS` の表を出し、AC の抜き差しで CPU に通知を送り始める（受け手が無い）。
- 選択肢:
  - A（推し）: 何も申告しない（0）。それでも 5330 は `_CPC` を出す（platform の `_OSC` の bit で決まるため）。
  - B: zedBSD の実際の動き（MWAIT の C-state）に合う bit だけを申告する（Intel の文書が要る）。
  - C: Linux と同じ bit を申告する。
- 推しの理由: 使わない物を使うと申告しない。目標の `_CPC` は A で得られる。

## Q310-4 PCIe の `_OSC` で OS が制御を得る範囲

- 背景: PCIe の `_OSC` の control の bit で、OS は PCIe の機能の担当を firmware から受け取る。5330 は hotplug と PME（wake の合図）の bit を求めた時点で firmware の処理を止める（design §3.3）が、zedBSD に代わりの処理は無い。WS300（NVMe と VMD の低電力）の ASPM・LTR は「PCIe Capability」と「LTR」の制御が要る（WS300 の Q300-6 と同じ話）。
- 選択肢:
  - A（推し）: PCIe Capability と LTR の 2 つだけ求める。hotplug・PME・AER・DPC は求めない。
  - B: 何も求めない（support の申告だけ）。WS300 の ASPM は `_OSC` 無しのまま（Q300-6 の判断に戻る）。
  - C: Linux と同じく hotplug・PME・AER も求める（zedBSD に代わりの処理を作るまで 5330 の wake・hotplug が壊れる見込み）。
- 推しの理由: zedBSD が実際に扱う物だけを受け取り、firmware の wake の処理を残す。Q300-6 の答えの形にもなる。

## Q310-5 一晩の耐久の試験

- 背景: AGENTS §9 で、負荷・耐久の試験はユーザーに確かめてから夜間に行う。5330 で `acpi.osc=on` のまま idle と S0 idle の往復を一晩続け、hang・再起動・新しい error が無いかを見る（design §6.3 の最後の行）。5330 には遠隔の電源が無い。
- 選択肢:
  - A（推し）: p006 の後の 1 晩、5330 で行う（止まっていたら朝に Q1 が知らせる）。
  - B: 行わない（p006 の 3 回の起動の比べだけで受け入れる）。
  - C: 時間を短く（2 時間など）昼に行う。
- 推しの理由: 「厳重に確かめる」の決定に合う。遠隔の電源が無いので夜間の停止は朝まで残るが、他の試験とぶつからない。

## Q310-6 ACPI 6.5 などの仕様の入手

- cockpit の 01（WS333 で ACPI 6.5 を手で置く件）と同じ件。この WS では ACPI 6.5 §6.2.11（`_OSC` と platform-wide の bit の表）、PCI Firmware Specification（PCIe の `_OSC` の bit）、Intel の processor の `_OSC` の文書が要る。今は 5330 の firmware の使い方と Linux の起動の log の bit の名前で裏付けた bit だけを使い、code の merge の前に仕様で確かめる（design §9）。
- 推し: 01 の答えに合わせる。手で置くなら ACPI 6.5 の PDF に加えて PCI Firmware Specification 3.3 も（PCI-SIG の会員向け。手に入らなければ ACPI と firmware の使い方で進める）。
