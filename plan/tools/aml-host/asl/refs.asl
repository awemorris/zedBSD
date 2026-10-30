/*
 * WS049 AML test: references, aliases and objects passed to methods.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "REFS", 1)
{
    Name (FAIL, 0)
    Name (NUM1, 10)
    Name (NUM2, 20)
    Name (TEXT, "named")
    Name (PKGR, Package () { NUM1, NUM2, TEXT })
    Name (BUFR, Buffer () { 1, 2, 3, 4 })
    Alias (NUM1, ALS1)
    Device (DEV1)
    {
        Name (_HID, "ZED0001")
        Name (VAL, 0x55)
        Method (GETV)
        {
            Return (VAL)
        }
    }

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

    /* Stores through the reference it is given. */
    Method (SETR, 2)
    {
        Store (Arg1, Arg0)
    }

    /* Reads through the reference it is given. */
    Method (GETR, 1)
    {
        Return (DerefOf (Arg0))
    }

    /* Changes a buffer it is given; buffers are shared with the caller. */
    Method (CHGB, 1)
    {
        Store (0xEE, Index (Arg0, 0))
    }

    /* Replaces its own argument; the caller's integer is not changed. */
    Method (CHGI, 1)
    {
        Store (99, Arg0)
        Return (Arg0)
    }

    Method (MAIN)
    {
        /* RefOf and DerefOf of named objects. */
        Store (RefOf (NUM1), Local0)
        CHK (DerefOf (Local0), 10, 1)
        CHK (ObjectType (Local0), 1, 2)
        Store (RefOf (TEXT), Local1)
        CHK (DerefOf (Local1), "named", 3)

        /* Storing through a reference argument changes the named object. */
        SETR (RefOf (NUM2), 21)
        CHK (NUM2, 21, 4)
        CHK (GETR (RefOf (NUM2)), 21, 5)

        /* The name is set directly: a store to DerefOf is not portable. */
        Store (22, NUM2)
        CHK (NUM2, 22, 6)

        /* CondRefOf of names that exist and that do not. */
        CHK (CondRefOf (NUM1), Ones, 10)
        CHK (CondRefOf (NUM1, Local2), Ones, 11)
        CHK (DerefOf (Local2), 10, 12)
        CHK (CondRefOf (\_SB.NOPE), 0, 13)
        CHK (CondRefOf (\DEV1.VAL), Ones, 14)

        /* A package element that names an object reads as its value. */
        CHK (DerefOf (Index (PKGR, 0)), 10, 20)
        CHK (DerefOf (Index (PKGR, 2)), "named", 21)

        /* An alias reads and writes the aliased object. */
        CHK (ALS1, 10, 30)
        Store (11, ALS1)
        CHK (NUM1, 11, 31)
        CHK (ObjectType (ALS1), 1, 32)

        /* Objects of a device are reached by path and by method. */
        CHK (\DEV1.VAL, 0x55, 40)
        CHK (\DEV1.GETV (), 0x55, 41)
        CHK (ObjectType (DEV1), 6, 42)
        Store (0x56, \DEV1.VAL)
        CHK (\DEV1.GETV (), 0x56, 43)

        /* Buffers passed to methods are shared; integers are not. */
        CHGB (BUFR)
        CHK (DerefOf (Index (BUFR, 0)), 0xEE, 50)
        Store (5, Local3)
        CHK (CHGI (Local3), 99, 51)
        CHK (Local3, 5, 52)

        /* An Index reference kept in a local reads and writes the element. */
        Index (BUFR, 3, Local4)
        CHK (DerefOf (Local4), 4, 60)
        Store (0x44, Local4)
        CHK (Local4, 0x44, 61)
        CHK (DerefOf (Index (BUFR, 3)), 4, 62)
        Store (Index (BUFR, 2), Local5)
        Store (0x33, Index (BUFR, 2))
        CHK (DerefOf (Local5), 0x33, 63)

        /* DerefOf of a string path. */
        Store ("\\NUM2", Local6)
        CHK (DerefOf (Local6), 22, 70)

        Return (FAIL)
    }
}
