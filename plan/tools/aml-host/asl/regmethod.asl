/*
 * WS049 AML test: _REG, which tells firmware that an address space's
 * handler is connected.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  aml-host runs it with --reg (regmethod.args), which
 * connects the spaces after loading as the kernel does; acpiexec does so
 * during its own initialization.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "REGMETH", 1)
{
    Name (FAIL, 0)
    Name (REGM, 0)
    Name (REGE, 0)
    Name (REGP, 0)
    Name (LAST, 0xFF)

    Device (EC0)
    {
        Name (_HID, EisaId ("PNP0C09"))
        OperationRegion (ECOR, EmbeddedControl, 0, 0x100)
        Field (ECOR, ByteAcc, Lock, Preserve)
        {
            ECB0, 8
        }

        /* Counts the connections of the embedded controller space. */
        Method (_REG, 2)
        {
            If (LEqual (Arg0, 3))
            {
                Store (Arg1, REGE)
            }
            Store (Arg0, LAST)
        }
    }

    Device (MDEV)
    {
        Name (_HID, "ZED0002")
        OperationRegion (MREG, SystemMemory, 0x00300000, 0x10)
        Field (MREG, ByteAcc, NoLock, Preserve)
        {
            MB0, 8
        }

        /* Counts the connections of system memory. */
        Method (_REG, 2)
        {
            If (LEqual (Arg0, 0))
            {
                Add (REGM, Arg1, REGM)
            }
        }
    }

    Device (PDEV)
    {
        Name (_ADR, 0x00040000)
        OperationRegion (PREG, PCI_Config, 0, 0x100)
        Field (PREG, DWordAcc, NoLock, Preserve)
        {
            PVID, 32
        }

        /* Counts the connections of PCI configuration space. */
        Method (_REG, 2)
        {
            If (LEqual (Arg0, 2))
            {
                Add (REGP, Arg1, REGP)
            }
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

    Method (MAIN)
    {
        /*
         * The embedded controller's _REG ran; system memory's did not,
         * because it is always available.  zedBSD also runs PCI_Config's
         * _REG when the PCI handler is installed (ACPI 6.5 section 6.5.4);
         * acpiexec does not, so REGP is not checked here.
         */
        CHK (REGE, 1, 1)
        CHK (REGM, 0, 2)
        CHK (LAST, 3, 4)
        Return (FAIL)
    }
}
