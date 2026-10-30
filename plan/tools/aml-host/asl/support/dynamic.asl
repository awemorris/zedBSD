/*
 * WS049 support table: an SSDT that load.asl and loadtable.asl load at
 * run time.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 */
DefinitionBlock ("", "SSDT", 2, "ZEDBSD", "DYNTBL", 1)
{
    External (\DYNP, IntObj)

    Name (\DYNV, 0x1234)

    Method (\DYNM, 1)
    {
        Return (Add (Arg0, 1))
    }

    Scope (\_SB)
    {
        Device (DYND)
        {
            Name (_HID, "ZED0003")
        }
    }
}
