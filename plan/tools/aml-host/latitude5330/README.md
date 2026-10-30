# Dell Latitude 5330 の DSDT・SSDT（ws049-p008、BUG-165）

host の試験（[check-latitude5330.sh](../check-latitude5330.sh)）の入力。2026-10-04 に 10.0.30.3（5330 の Linux、BIOS 1.31.1）から読み取り専用で
`sudo cat /sys/firmware/acpi/tables/DSDT`・`SSDT1`〜`SSDT15` で取り出した（user の許可 2026-10-04 17 時、[BUG-165](../../../bugs/BUG-165.md) の「許可」）。
番号は sysfs の番号（firmware の root table の順）。他の table（FACP・MSDM など）は取り出していない。

firmware の table は Dell の著作物であり、zedBSD の source ではない。試験の入力として置くだけで、image にも配布物にも入れない。

| file | OEM Table ID | sha256 |
| --- | --- | --- |
| dsdt.dat | `Dell Inc`（OEM ID `DELL`） | 9dfff44ebb80ddee14b2137e03882bacb028a1e46826061134a4e1ce48633edf |
| ssdt1.dat | `Pmax_Dev` | 7d9bf2acf8e4b1bdcc6c1654edc3e77a5ec9e6a08f50795431c8797e0c054c23 |
| ssdt2.dat | `CpuSsdt` | 85124a1ae9c73b0db6cab5b0fd2a8b59ca7dc1993cd547a9b4a01225545e7e99 |
| ssdt3.dat | `DptfTabl` | 80ab69670ac70e02b4d39a572bafc31adec454cdc6afb3a88122169ae985559e |
| ssdt4.dat | `DellRtd3` | 86479ba4b6c281edd98f85d368a8b60e919410fd81ca91dc2584a504bd0b5fee |
| ssdt5.dat | `SaSsdt` | 4082fbac0f9a83e7c37499243b457c2d653c9bae8131fbdf362c552ff59268f2 |
| ssdt6.dat | `IgfxSsdt` | 7442395c2882ea1984401b97158bb66d0094f4bd6560818bd3ee281721a4e1c6 |
| ssdt7.dat | `TcssSsdt` | a2a55522ddb9d1b9826c25b4f3552b9d80677a09e2ecec08286dd5777d8f5d54 |
| ssdt8.dat | `UsbCTabl` | d8faba60e864af73466fed93894e2d71018a24942f0f34719d6a6ca0df1b3247 |
| ssdt9.dat | `PtidDevc` | 84cc7f6c1f5b4ef15ea7c9d6f7356dd405eb41f4c65dc0b5613fbf9eea4d440e |
| ssdt10.dat | `TbtTypeC` | 6fb0ba0bc01ed1c9176f1088d2eef0cd61d422460dc4f2206bc64f1845567aa7 |
| ssdt11.dat | `Tpm2Tabl` | f242a40c05f2542d9fa43b49fe12bc8930f4e0c53d6be25bca94c296434991c1 |
| ssdt12.dat | `xh_Dell_` | e12f29262d7cae23de7656162e435453b46a5a9c9233740c5e3402f4dfbc0c0b |
| ssdt13.dat | `SocGpe` | a761335ea985397defb4d5b7151d7d658459e6366c7bc46d822ee53b49a41f98 |
| ssdt14.dat | `SocCmn` | f2028ce43dc53c1f1944a5f4650c58ea9ac50d1919299a09fdc8214ed88b5c22 |
| ssdt15.dat | `ADebTabl` | 96cc8839b0e1cc5dd75536f89553d00737652429fbca535227ad8fb68309a387 |

読むには `iasl -d dsdt.dat`（acpica-tools）。

## FACP・LPIT と動的な SSDT（ws052-p002、2026-10-05）

2026-10-05 に同じ 10.0.30.3 から読み取り専用で `sudo cat /sys/firmware/acpi/tables/FACP`・`LPIT`・`dynamic/SSDT16`〜`SSDT23` で取り出した
（同じ user の許可の範囲、Q1 の指示 q727）。host の設定は変えていない。動的な SSDT は CPU の SSDT（`CpuSsdt`）が `_PDC`・`_OSC` の後に
`Load` する table（`_CST`・`_PSS`・`_PSD`・HWP）で、sysfs の番号のまま `dynamic/` に置く。

| file | 内容 | sha256 |
| --- | --- | --- |
| facp.dat | FADT（revision 6、PM Profile Mobile、SCI 9、flags 0x0020C4F5: **Low Power S0 Idle = 1**、Hardware Reduced = 0） | e8e21d625dcb6722b82c6b5135447dc1de7dbc94e01dae135d122944a3299f95 |
| lpit.dat | LPIT（3 つの Native C-state、下） | f9ecb17e2b7eae8734176fe09440245ebc9575a2ca21a845f1a321e8c53d6c35 |
| dynamic/ssdt16.dat | `Cpu0Cst`（`\_SB.PR00._CST`） | a4a961e1fb42b8e74d0a494618433e555137e6923ae2d58be44fb0c379840473 |
| dynamic/ssdt17.dat | `Cpu0Ist` | ffc6f688d4e02acfce83955db42ccd03d506d9b1e481442a03f391ede918fb80 |
| dynamic/ssdt18.dat | `Cpu0Psd` | ba3e9f8de1ad969d880705a046efd974629431e9c3a6c1ece3a07468538d53b9 |
| dynamic/ssdt19.dat | `Cpu0Hwp` | 256b2e6d6b65f0483620a3a406ea0c2249d5c59a7799bb181220e3ab6fa13ccc |
| dynamic/ssdt20.dat | `ApIst` | d055d1a921660c8a0ee608ef5a1251f6e12cb895acbec78c95313ee093f0c52e |
| dynamic/ssdt21.dat | `ApHwp` | 472267a40e3b45ae67daf8acfeb925a61eae97d39e9b5fdf0e4ea27a9e2084fc |
| dynamic/ssdt22.dat | `ApPsd` | be2ffd80acec715c990db021e3b971bcb377dcef1b9e9eb3dc2b13e4bfc6d150 |
| dynamic/ssdt23.dat | `ApCst` | 2865798497440102e27730660949b1ebb2daa97c612d83e4af6dc44490facf0b |

