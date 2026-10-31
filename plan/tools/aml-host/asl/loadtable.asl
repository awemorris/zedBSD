/*
 * WS049 AML test: LoadTable and Unload.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  LoadTable finds tables the firmware lists but did not
 * load; aml-host gets support/dynamic.aml that way (loadtable.args).
 * acpiexec loads every table it is given, so it cannot run this test
 * (loadtable.harness-only).
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "LDTBL", 1)
{
    External (\DYNV, IntObj)
    External (\DYNM, MethodObj)

    Name (FAIL, 0)
    Name (DYNP, 0)
    Name (HNDL, 0)

    Method (CHK, 3)
    {
        If (LNotEqual (Arg0, Arg1))
        {
            If (LEqual (FAIL, 0))
            {
                Store (Arg2, FAIL)
            }
        }
    }

    /* Loads the table a second time, which must fail. */
    Method (LDUP)
    {
        LoadTable ("SSDT", "ZEDBSD", "DYNTBL", "", "", 0)
        LoadTable ("SSDT", "ZEDBSD", "DYNTBL", "", "", 0)
        Return (0)
    }

    Method (MAIN)
    {
        /* A table the firmware does not list gives zero. */
        CHK (LoadTable ("SSDT", "ZEDBSD", "NOTHERE", "", "", 0), 0, 1)

        /* Loads the table and stores the parameter. */
        Store (LoadTable ("SSDT", "ZEDBSD", "DYNTBL", "\\", "\\DYNP", 0x77), Local0)
        CHK (ObjectType (Local0), 15, 2)
        CHK (DYNP, 0x77, 3)
        CHK (\DYNV, 0x1234, 4)
        CHK (\DYNM (1), 2, 5)
        CHK (CondRefOf (\_SB.DYND), Ones, 6)

        /* Unloads it: its names are gone and the rest stays. */
        Unload (Local0)
        CHK (CondRefOf (\DYNV), 0, 10)
        CHK (CondRefOf (\_SB.DYND), 0, 11)
        CHK (CondRefOf (\_SB), Ones, 12)
        CHK (DYNP, 0x77, 13)

        /* It can be loaded again after the Unload. */
        Store (LoadTable ("SSDT", "ZEDBSD", "DYNTBL", "", "", 0), Local1)
        CHK (\DYNV, 0x1234, 20)
        Unload (Local1)

        Return (FAIL)
    }
}
