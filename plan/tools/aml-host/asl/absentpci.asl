/*
 * WS049 AML test: PCI_Config regions of a function that is not there
 * (BUG-165).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  absentpci.args names 0:10.6 absent: like the PCI bus, and
 * like the kernel since BUG-165, its configuration space reads as all ones
 * and drops writes.  The table tests the vendor ID at the top level, as the
 * Latitude 5330's DSDT does for its THC0 (0:10.6, disabled by the BIOS),
 * and the definitions after that test must still be made: the kernel used
 * to fail the access and stop the whole DSDT there.  acpiexec simulates
 * every function as memory that reads zero, so it does not run this test.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "ABSNTPCI", 1)
{
    Scope (\_SB)
    {
        Device (PC00)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, EisaId ("PNP0A03"))
            Name (_BBN, Zero)

            Device (THC0)
            {
                Name (_ADR, 0x00100006)
                OperationRegion (THCR, PCI_Config, Zero, 0x0100)
                Field (THCR, ByteAcc, NoLock, Preserve)
                {
                    VDID, 32,
                    CMDR, 8
                }
                Field (THCR, QWordAcc, NoLock, Preserve)
                {
                    Offset (0x10),
                    BAR0, 64
                }

                /* Only a function that is there gets its methods. */
                If ((VDID != 0xFFFFFFFF))
                {
                    Name (PRSN, One)
                }
            }

            Device (XHCI)
            {
                Name (_ADR, 0x00140000)
                OperationRegion (XPRT, PCI_Config, Zero, 0x0100)
                Field (XPRT, ByteAcc, NoLock, Preserve)
                {
                    DVID, 16,
                    Offset (0x04),
                    XCMD, 8
                }
            }
        }
    }

    /* Made only when the table runs past the absent function. */
    Name (LAST, 0x1234)

    Method (MAIN, 0, NotSerialized)
    {
        /* The absent function's vendor and device read as all ones. */
        If ((\_SB.PC00.THC0.VDID != 0xFFFFFFFF))
        {
            Return (1)
        }

        /* A write to it is dropped: the byte still reads as all ones. */
        \_SB.PC00.THC0.CMDR = 0x07
        If ((\_SB.PC00.THC0.CMDR != 0xFF))
        {
            Return (2)
        }

        /* The top-level test skipped the absent device's definitions. */
        If (CondRefOf (\_SB.PC00.THC0.PRSN))
        {
            Return (3)
        }

        /* A 64-bit access reads every bit set. */
        If ((\_SB.PC00.THC0.BAR0 != 0xFFFFFFFFFFFFFFFF))
        {
            Return (4)
        }

        /* A function that is there keeps what is written to it. */
        \_SB.PC00.XHCI.XCMD = 0x06
        If ((\_SB.PC00.XHCI.XCMD != 0x06))
        {
            Return (5)
        }

        /* The table ran to its end. */
        If ((LAST != 0x1234))
        {
            Return (6)
        }

        Return (0)
    }
}
