/*
 * WS049 AML test: operation regions and field units.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  The address spaces are simulated as memory that reads zero
 * until written (aml-host and acpiexec both do that), so an index register
 * and its data register behave as plain bytes.  Every check stays inside
 * one region: acpiexec keeps a separate buffer for each region.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "REGION", 1)
{
    Name (FAIL, 0)
    Name (MBAS, 0x00100000)

    OperationRegion (MEM0, SystemMemory, MBAS, 0x100)
    Field (MEM0, ByteAcc, NoLock, Preserve)
    {
        B000, 8,
        B001, 8,
        B002, 8,
        B003, 8,
        Offset (0x10),
        N10L, 4,
        N10H, 4,
        X11, 12,
        Offset (0x20),
        WIDE, 128,
        Offset (0x40),
        R40, 32,
        Offset (0x48),
        R48, 32,
        Offset (0x60),
        R60, 16
    }
    Field (MEM0, DWordAcc, NoLock, Preserve)
    {
        D000, 32,
        Offset (0x10),
        D010, 32
    }
    Field (MEM0, DWordAcc, NoLock, WriteAsOnes)
    {
        Offset (0x40),
        P40, 4
    }
    Field (MEM0, DWordAcc, NoLock, WriteAsZeros)
    {
        Offset (0x48),
        Z48, 4
    }
    Field (MEM0, WordAcc, NoLock, Preserve)
    {
        Offset (0x60),
        W60, 8,
        W61, 8
    }
    Field (MEM0, AnyAcc, NoLock, Preserve)
    {
        Offset (0x70),
        A70, 16,
        A72, 3,
        A72X, 13
    }

    OperationRegion (IO0, SystemIO, 0x0800, 0x10)
    Field (IO0, ByteAcc, NoLock, Preserve)
    {
        IDX, 8,
        DAT, 8,
        BNK, 8,
        Offset (0x04),
        IO4, 8
    }
    IndexField (IDX, DAT, ByteAcc, NoLock, Preserve)
    {
        Offset (0x10),
        IF10, 8,
        IF11, 8
    }
    BankField (IO0, BNK, 1, ByteAcc, NoLock, Preserve)
    {
        Offset (0x04),
        BK1, 8
    }
    BankField (IO0, BNK, 2, ByteAcc, NoLock, Preserve)
    {
        Offset (0x04),
        BK2, 8
    }

    Device (PCI0)
    {
        Name (_HID, EisaId ("PNP0A08"))
        Name (_BBN, 0)
        Device (DEV3)
        {
            Name (_ADR, 0x00030001)
            OperationRegion (CFG, PCI_Config, 0, 0x100)
            Field (CFG, DWordAcc, NoLock, Preserve)
            {
                VDID, 32,
                Offset (0x40),
                C40, 8,
                C41, 8
            }
        }
    }

    DataTableRegion (DTR, "DSDT", "", "")
    Field (DTR, AnyAcc, NoLock, Preserve)
    {
        SIGN, 32,
        TLEN, 32
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

    /* A region whose place comes from the method's argument. */
    Method (LOCR, 1, Serialized)
    {
        OperationRegion (LRGN, SystemMemory, Arg0, 8)
        Field (LRGN, ByteAcc, NoLock, Preserve)
        {
            L0, 8,
            L1, 8
        }
        Store (0x99, L0)
        Store (Add (L0, 1), L1)
        Return (L1)
    }

    Method (MAIN)
    {
        /* Byte fields and a dword view of the same bytes. */
        Store (0x11, B000)
        Store (0x22, B001)
        Store (0x33, B002)
        Store (0x44, B003)
        CHK (D000, 0x44332211, 1)
        Store (0xAABBCCDD, D000)
        CHK (B000, 0xDD, 2)
        CHK (B003, 0xAA, 3)

        /* Sub-byte fields and a field across bytes keep their neighbors. */
        Store (0x5, N10L)
        Store (0xA, N10H)
        CHK (D010, 0xA5, 4)
        Store (0xFFF, X11)
        CHK (D010, 0xFFFA5, 5)
        Store (0x123, X11)
        CHK (D010, 0x123A5, 6)
        CHK (N10H, 0xA, 7)
        CHK (X11, 0x123, 8)

        /* A field wider than an integer reads and writes as a buffer. */
        Store (Buffer () { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, WIDE)
        CHK (WIDE, Buffer () { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 }, 9)
        CHK (ObjectType (WIDE), 5, 10)
        Store (0x0201, WIDE)
        CHK (WIDE, Buffer () { 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 }, 11)

        /* The update rules fill the rest of a wider access. */
        Store (0x3, P40)
        CHK (R40, 0xFFFFFFF3, 20)
        Store (0xFFFFFFFF, R48)
        Store (0x5, Z48)
        CHK (R48, 0x5, 21)
        Store (0x1234, R60)
        Store (0x77, W61)
        CHK (R60, 0x7734, 22)
        CHK (W60, 0x34, 23)

        /* Any access. */
        Store (0xBEEF, A70)
        Store (0x5, A72)
        CHK (A70, 0xBEEF, 24)
        CHK (A72, 0x5, 25)
        CHK (A72X, 0, 26)

        /* An index field writes the index, then the data register. */
        Store (0x5A, IF10)
        CHK (IDX, 0x10, 30)
        CHK (DAT, 0x5A, 31)
        CHK (IF11, 0x5A, 32)
        CHK (IDX, 0x11, 33)

        /* A bank field selects its bank first. */
        Store (0x66, BK1)
        CHK (BNK, 1, 40)
        CHK (IO4, 0x66, 41)
        CHK (BK2, 0x66, 42)
        CHK (BNK, 2, 43)

        /* PCI configuration space of the device the region is in. */
        CHK (\PCI0.DEV3.VDID, 0, 50)
        Store (0x12, \PCI0.DEV3.C40)
        Store (0x34, \PCI0.DEV3.C41)
        CHK (\PCI0.DEV3.C40, 0x12, 51)
        CHK (\PCI0.DEV3.C41, 0x34, 52)

        /* A data table region reads the table. */
        CHK (SIGN, 0x54445344, 60)
        CHK (LGreater (TLEN, 0x100), Ones, 61)

        /* A region made in a method from its argument. */
        CHK (LOCR (0x00200000), 0x9A, 70)

        Return (FAIL)
    }
}
