<!-- awesome-plan project=zedbsd record=ws327-p001 -->

# ws327-p001 設計（thread と network の層、Win32 への写し、移す app の棚卸し）

- 親: [WS327](../ws.md)。設計: [design.md](../design.md)。Queue: q1697 / q1697-i01（担当 P2、2026-10-30）。
- Status: cleared（2026-10-30。§12 の 3 つの問いに Q1 が答えた: 104 を使う、Win32 の cross は範囲の外（p006 の案）、plan/tools/ の host 試験の修正は範囲に追加）
- Disposition: normal

## 目的・受け入れ

- `keiland_thread.h`・`keiland_socket.h` の案、各関数の約束、Win32 への写しの表、移す app の一覧（grep の棚卸し）、design-reviewer 1 本。

## 結果

- [design.md](../design.md) §0〜§12。棚卸しは c90327de3 の tree の grep（§1）。Terminal・Installer・Phone は WS314 の後 thread・socket を直に使わない。
- design-reviewer（2026-10-30）: 18 件（high 1・medium 8・low-medium 3・low 6）。全て §11 のとおり反映。high の 1（waker の順で鳴りが永久に失われる）は
  実装を正しい順で書き、試験 `waker-no-lost-ring` が逆の順の mutant で FAIL する事を確かめた（p003）。
- Q1 の答え（§12）: KL_VERSION 104 を使う（次は 105）、Win32 の cross の compile と wine は範囲の外（p006 の案）、`plan/tools/mail`・`music`・`media` の host 試験の link の修正は q1697 の範囲に追加。
