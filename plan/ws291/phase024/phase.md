<!-- awesome-plan project=zedbsd record=ws291-p024 -->

# ws291-p024: NVIDIA: subs-1000 の GPU の 1 回の実行ごとの固定の費用（約 56 µs）を内訳ごとに測って縮める

Status: in-progress（2026-10-31 21:04 P3 q1785 で着手）
Parent: [WS291](../ws.md)
依存: [p023](../phase023/phase.md)（block-linear の置き方で pass ごとの copy が消えた後）

## 目的

2026-10-31 ユーザーから、p023 の実装の後に続けて subs-1000 の固定の費用の内訳を測る、という趣旨の答えを受けた（p023 の Q2 の (b)）。
p014 の nc の診断で、copy を除いた GPU の 1 run は約 60 µs（描画は約 4 µs）、NVK は 1 submit 約 5〜7 µs。

## 範囲（案、Queue を立てる時に確定）

1. run ごとの固定の段（texture pool と shader cache の invalidate、最初の draw の全 state、pass の始めと終わりの barrier の WFI、run の終わりの WFI の fence、
   channel の切り替えの有無）を 1 つずつ外した変種で測る（p014 の rtx-runcost.sh）。
2. 外せる物・軽くできる物を直し、subs-100・subs-1000・big-1000・scene・desktop の後退と画素の比べ。HAL は変えない。

## ユーザーの指示（2026-10-31）

- 2026-10-31 ユーザーから、P3 は RTX での描画の最適化を目標の達成まで進める、という趣旨の指示を受けた。目標は p013・p014 の NVK 並み（big-1000 136.2 fps、subs-1000 75.4 fps）。
  この Queue では p024（subs の 1 run の固定の費用）に加え、big-1000 の残りの差と、zero copy の game mode での big-100 の約 20% の遅れも扱う。

## q1785-i01（2026-10-31 P3、worktree p3-ws291wsi、main b689bbf66 を ad4e8876b で取り込み）

- 承認: Queue q1785-i01（上のユーザーの指示）。実機 10.0.10.3（RTX 2070）、lock /tmp/zedbsd-rdna.lock を 21:12 から window ごとに取る。
  計測の userland は全部この tree の 14 file（`make ZEDBSD_CONFIG=plan/ws291/tests/config.mk BUILD=build/p024/base` の個別の target、
  base の kernel 3084829296 は main の RTX kernel と cksum 一致）、kernel だけを入れ替える。各 window の終わりに 14 file と NVMe の kernel を戻して
  cksum 一致（vmunix-va1 1463007375、cfg の nvrtx.vk.perf 行 0）、Debian（RTX 4 function vfio-pci）。

### 1 run の固定の費用の内訳（rtx-runcost.sh、診断 kernel、21:12〜21:25）

- 診断の差分 `plan/ws291/tests/nvrtx-run-cost-diag.patch`（今の tree 向けに作り直し、session ごとに 8 つの変種を順に）: 0 そのまま、1 push buffer の
  始めの shader cache の invalidate なし、2 barrier の WFI なし、3 run の fence の WFI なし、4 fence の membar と non-stall 割り込みなし、
  5 record を 1 つも実行しない（空の run）、6 = 1+2+3+4、7 = 5+3+4。`rtx-runcost.sh` に `TIMING=on|off` を足した。
- subs-1000（timing off、16 run、build/tmp/p024-d1、worktree の build、git の外）: 0・1・2・4・6 は 19.8〜20.6 fps（submit 42〜43.5 ms/frame）、3 は 21.7、
  **5・7（空の run）でも 23.7〜24.4 fps（submit 34.3〜35.2 ms = 1 submit 約 34 µs）**。GPU の中身を全部外しても 1 submit 約 34 µs が残る。
- subs-100（timing on、16 run、build/tmp/p024-d2）: GPU の 1 run は 0 で約 20 µs、空の run で 4 µs。executor が GPU を待った時間（blocked）は 5 s に
  0.1〜0.2 s（2〜4 %）。→ subs の律速は GPU ではなく CPU の 1 submit の道（約 34〜40 µs）。p014 の「GPU の 1 run 約 56 µs」は、copy と timed の
  end stamp の WFI を含んだ値で、今は GPU の余裕の方が大きい。

### CPU の 1 submit の内訳（libvulkan の VULKAN_ZEDBSD_SUBMIT_TIMING、rtx-p024.sh、21:25〜21:33、build/tmp/p024-ab1）

- base の subs-1000（timing 自体が 1 部分 約 2 µs かかるので fps は 12.4 に落ちる）: vkQueueSubmit 75 µs = reserve（GPU_JOB_RESERVE）7・execute 46
  （GPU_COMMAND の ioctl 41、command_ran 61923 回・deferred 24 回 = ほぼ全部が呼び手の thread で同期に走った）・finish（GPU_JOB_COMMIT）3・
  private_fence 2・reap（8677 回、1 回 16 µs）。
- c2（下の USERD の put を 1 submit に 1 回）で GPU_COMMAND の ioctl 41 → 31 µs。register window（PRAMIN）の書き 1 回は rd32 2 回と wr32 3 回で約 5 µs、
  1 submit に 3 回書いていた。register の読みが 1 回 約 2 µs かかる。
