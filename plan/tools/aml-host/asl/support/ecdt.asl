/*
 * WS049 test table: an ECDT for ecdt.asl's EC (ports 0x66/0x62, GPE 0x16,
 * \_SB.EC0), in iasl's data table form.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
[0004]                          Signature : "ECDT"    [Embedded Controller Boot Resources Table]
[0004]                       Table Length : 00000000
[0001]                           Revision : 01
[0001]                           Checksum : 00
[0006]                             Oem ID : "ZEDBSD"
[0008]                       Oem Table ID : "ECDT    "
[0004]                       Oem Revision : 00000001
[0004]                    Asl Compiler ID : "INTL"
[0004]              Asl Compiler Revision : 20250404

[0012]            Command/Status Register : [Generic Address Structure]
[0001]                           Space ID : 01 [SystemIO]
[0001]                          Bit Width : 08
[0001]                         Bit Offset : 00
[0001]               Encoded Access Width : 00 [Undefined/Legacy]
[0008]                            Address : 0000000000000066

[0012]                      Data Register : [Generic Address Structure]
[0001]                           Space ID : 01 [SystemIO]
[0001]                          Bit Width : 08
[0001]                         Bit Offset : 00
[0001]               Encoded Access Width : 00 [Undefined/Legacy]
[0008]                            Address : 0000000000000062

[0004]                                UID : 00000000
[0001]                         GPE Number : 16
[0001]                           Namepath : "\_SB.EC0"
