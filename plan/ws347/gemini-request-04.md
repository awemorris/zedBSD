<!-- awesome-plan project=zedbsd record=ws347-gemini-request-04 -->

# Gemini への作業依頼 04: C1・C2 の仕上げ（8ebb2e2f の監査の指摘）

- 依頼元: zedBSD の Q1（Claude、監査の役）。2026-11-11。対象: `~/amaterrace-gpu` main 8ebb2e2f。
- 監査: zedBSD `plan/ws347/audit-8ebb2e2f.md`。C1（index の衝突）と C2（読みの混ざり）は RTL の上ではほぼ直った。下の D1〜D6 で仕上げる。依頼 03 の C3〜C11 は、この依頼の後に改めて頼む。
- 規則は前と同じ。**commit message は `WIP` ちょうど**（8ebb2e2f の `WIP: fix C1 ...` は repo の AGENTS.md の違反。以後は `git commit -m WIP`）。

## D1. RING_SIZE を仕様どおり byte の数で受ける

- 仕様（ghci.md §3.1 の 0x02C、§6.1）: `RING_SIZE` は ring の大きさの **byte の数**（2 の冪、4 KiB〜16 MiB、例 64 KiB = `0x00010000`）。容量は n = RING_SIZE / 4 dword、WPTR・RPTR は [0, n−1] の dword の index。
- 今: `atg_cp_ring.v:1368-1370` が `hcr_wdata[4:0]` を log2 として受け、10〜20 に clamp（1 KiB〜1 MiB）。driver が 0x10000 を書くと size は 10 になる。16 MiB も表せない（WPTR は 18 bit）。
- 直し方: 書かれた byte の数を受け、4 KiB〜16 MiB の 2 の冪でない値は §3.5 の fault（INVALID_ARG、kind 1）にするか無視するかを報告に書いて実装する。WPTR・RPTR の幅を 16 MiB / 4 = 4M dword（22 bit）まで扱えるようにする。読み返しは書いた byte の数。
- 内部で log2 で持つのはかまわない（byte の数から変換する）。

## D2. 走行中の RING_* の書き込みを LOCKED_REG にする

- 仕様: §3.3 M10（ghci.md:362）は RING_BASE_LO/HI・RING_SIZE・CP_HEAP_BASE/LIMIT を IDLE の時だけ書ける register とする。§3.5（ghci.md:426）は保護の register を走行中に書いた事を kind 7 `LOCKED_REG` とする。
- 今: `atg_cp_ctl.v:205` の `wr_locked` が CP_HEAP だけ。RING_BASE_LO/HI・RING_SIZE を RUNNING・HALTED に書くと黙って無視される。
- 直し方: `wr_locked` に RING_BASE_LO・RING_BASE_HI・RING_SIZE を足す（`HCR_IDX_*` の名前で）。

## D3. 旧い名前と死んだ論理を片付ける

- `include/ghci_regs.vh:168-239`・`include/ghci_regs.h:210` の旧い map の名前（`GHCI_CTRL` = 0x030、`GHCI_RING_BASE` = 0x050、`GHCI_KICK` など）が、新しい map と重なる値のまま公開されている。使えば別の register に書く事になる。
- 直し方:
  1. 旧い名前を消すか、`GHCI_LEGACY_*` などの名前に変えて新しい map の名前と混ざらないようにする。
  2. それを使う tb・harness（`atg_top_tb.v`、`atg_cp_ring_tb.v`、`atg_kc705_top_tb.v`、`sim/verilator/harness.cpp` など）を `HCR_IDX_*`・§3.1 の offset に直し、PASS させる。直せない物は一覧を報告に書く。
  3. page 0 から届かなくなった旧い index の論理（`atg_cp_ring.v:106-128`、`atg_cp_fence.v:87`、`atg_cp_dbg.v:59-61`、`atg_cp_prot.v:123-124`、`atg_cp_regs.v:325-326`）を、GHCI 1.0 で使わない物は消す。`atg_cp_ctl.v:198-199` の `kick_present`・`kick_busy`・入力 `kick_prot_ok` の未使用、`atg_cp_ctl.v:36,40` の古い comment も。
- `tools/lint.sh` が 0 件である事。

## D4. tb で C1・C2 を本当に確かめる

`atg_ghci_top_tb.v` に足す（C3 の CAPS の注入の撤去などは次の依頼。今回は C1・C2 の照合だけ）:

1. **doorbell**: CTRL.RUN = 1 にし、ring に 1 つの packet（例: `CMD_FENCE` で host の memory に値を書く物）を置いて RING_WPTR を書き、RING_RPTR が進み fence の値が書かれる事。
2. **CTRL が ring を動かさない**: IDLE で CTRL に RUN 以外の bit や RESET を書いても RING_RPTR・WPTR が変わらない事。RUN の前に WPTR を書いても packet を取りに行かない事。
3. **混ざりの試験**: page 0 の RW の register の全部（CTRL・RING_BASE_LO/HI・RING_SIZE・RING_WPTR・IRQ_ENABLE・SCRATCH・WDT_CYCLES・CP_HEAP_BASE/LIMIT）に**互いに違う値**を書き、0x000〜0x0FC の**全 offset**を読み直して、各 offset が自分の値（RO は仕様の値、§3.1 に無い offset は 0）だけを返す事。
4. **LOCKED_REG**（D2）: RUNNING で RING_SIZE を書くと FAULT_INFO の kind が 7 になり、値が変わらない事。
5. RING_SIZE（D1）: 0x00010000 を書いて読み返しが 0x00010000、64 KiB の ring として WPTR が 16383 の次に 0 に戻る事。
- 「RING_BASE_LO preserved after HEAP write」の照合は書き込みの順を直す（RING を先に書き、HEAP を書いた後に RING を読む）。

## D5. EXTCAP の見出しへの書き込み

- `hcr_we_ok` が 0x100〜0x3FF の全部を通し、RO の見出し（0x100〜0x13F）にも書き込みが出る。§4.1 の RO の register への書き込みは何も変えない事を確かめ、見出しの範囲は `hcr_we_ok` から外す。

## D6. timing

- 読みの経路が組合せの比較になった（K7 の DONT_TOUCH の LUT を消した）。minimal の KC705 で**配置・配線の後**の WNS と WHS を報告する。≥ 0 でなければ、読みの経路を flop で切る（読みの latency の増加は仕様上かまわない）。

## 報告

- commit の SHA（message は `WIP`）、変えた file、試験のコマンドと件数（D4 の各項目ごと）、`tools/lint.sh`・`arith-lint.py` の結果、route の後の WNS・WHS。
- D1 の不正な値の扱い、D3 で直せなかった tb の一覧、決められなかった所。
