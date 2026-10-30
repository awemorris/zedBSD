/*
 * WS049 AML test: buffer fields and resource templates.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "BUFFLD", 1)
{
    Name (FAIL, 0)
    Name (BUF0, Buffer (16) { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA })
    CreateBitField (BUF0, 0, BIT0)
    CreateBitField (BUF0, 9, BIT9)
    CreateByteField (BUF0, 1, BYT1)
    CreateWordField (BUF0, 2, WRD2)
    CreateDWordField (BUF0, 4, DWD4)
    CreateQWordField (BUF0, 8, QWD8)
    CreateField (BUF0, 4, 8, NIB4)
    CreateField (BUF0, 0, 96, WIDE)

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

    /* Builds a memory resource with a base patched in, as firmware _CRS methods do. */
    Method (MCRS, 1, Serialized)
    {
        Name (RBUF, ResourceTemplate ()
        {
            Memory32Fixed (ReadWrite, 0x00000000, 0x00001000, MEM0)
        })
        CreateDWordField (RBUF, \MCRS.MEM0._BAS, BASE)
        Store (Arg0, BASE)
        Return (RBUF)
    }

    Method (MAIN)
    {
        /* Reads of each width. */
        CHK (BIT0, 1, 1)
        CHK (BIT9, 1, 2)
        CHK (BYT1, 0x22, 3)
        CHK (WRD2, 0x4433, 4)
        CHK (DWD4, 0x88776655, 5)
        CHK (QWD8, 0xAA99, 6)
        CHK (NIB4, Buffer () { 0x21 }, 7)
        CHK (ObjectType (WIDE), 14, 8)

        /* Writes change the buffer and only the field's bits. */
        Store (0, BIT0)
        CHK (DerefOf (Index (BUF0, 0)), 0x10, 10)
        Store (0xFF, NIB4)
        CHK (DerefOf (Index (BUF0, 0)), 0xF0, 11)
        CHK (DerefOf (Index (BUF0, 1)), 0x2F, 12)
        Store (0xBEEF, WRD2)
        CHK (DerefOf (Index (BUF0, 2)), 0xEF, 13)
        CHK (DerefOf (Index (BUF0, 3)), 0xBE, 14)
        Store (0x123456789ABCDEF0, QWD8)
        CHK (DerefOf (Index (BUF0, 8)), 0xF0, 15)
        CHK (DerefOf (Index (BUF0, 15)), 0x12, 16)
        Store (0x1FFFF, WRD2)
        CHK (WRD2, 0xFFFF, 17)
        CHK (DerefOf (Index (BUF0, 4)), 0x55, 18)

        /* A buffer written into a wide field. */
        Store (Buffer () { 1, 2, 3 }, WIDE)
        CHK (DerefOf (Index (BUF0, 0)), 1, 20)
        CHK (DerefOf (Index (BUF0, 3)), 0, 21)
        CHK (DerefOf (Index (BUF0, 12)), 0x78, 22)

        /* Fields made in a method over a local buffer. */
        Store (Buffer (4) { 0, 0, 0, 0 }, Local0)
        CreateWordField (Local0, 1, LWRD)
        Store (0xABCD, LWRD)
        CHK (Local0, Buffer () { 0, 0xCD, 0xAB, 0 }, 30)

        /* A resource template patched by a method. */
        Store (MCRS (0xFED40000), Local1)
        CHK (SizeOf (Local1), 14, 40)
        CHK (DerefOf (Index (Local1, 4)), 0x00, 41)
        CHK (DerefOf (Index (Local1, 6)), 0xD4, 42)
        CHK (DerefOf (Index (Local1, 7)), 0xFE, 43)

        /* ConcatenateResTemplate keeps one end tag. */
        Store (ResourceTemplate () { IO (Decode16, 0x60, 0x60, 1, 1) }, Local2)
        Store (ResourceTemplate () { IRQNoFlags () { 1 } }, Local3)
        ConcatenateResTemplate (Local2, Local3, Local4)
        CHK (SizeOf (Local4), 13, 50)
        CHK (DerefOf (Index (Local4, 8)), 0x22, 51)
        CHK (DerefOf (Index (Local4, 11)), 0x79, 52)

        Return (FAIL)
    }
}
