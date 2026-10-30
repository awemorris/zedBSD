/*
 * WS049 AML test: the PCI function of a configuration region below
 * PCI-to-PCI bridges (ws049-p016).
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that failed.  The harness keeps each function's configuration space
 * apart, and pcibridge.args names 5:0.0 absent, so a region that resolves
 * to bus 5 reads all ones while one left on bus 0 reads what was written.
 * MAIN first makes \_SB.PC00.RP01 (0:1c.0) a bridge to bus 5 by writing
 * its header type and secondary bus, then reads the region of the device
 * below it.  \_SB.PC00.RP02 (0:1c.1) stays an ordinary function (header
 * type 0), so its child stays on bus 0, as on a host bridge.  The Latitude
 * 5330's \_SB.PC00.RPxx.PXSX regions are of this kind.  acpiexec keeps one
 * buffer per region address, so it does not run this test.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "PCIBRDG", 1)
{
    Scope (\_SB)
    {
        Device (PC00)
        {
            Name (_HID, EisaId ("PNP0A08"))
            Name (_CID, Package () { EisaId ("PNP0A03") })
            Name (_BBN, Zero)

            /* 0:0.0, the function a region below a bridge used to reach. */
            Device (HBRG)
            {
                Name (_ADR, Zero)
                OperationRegion (HBUS, PCI_Config, Zero, 0x0100)
                Field (HBUS, ByteAcc, NoLock, Preserve)
                {
                    HVID, 32
                }
            }

            Device (RP01)
            {
                Name (_ADR, 0x001C0000)
                OperationRegion (PXCS, PCI_Config, Zero, 0x0100)
                Field (PXCS, ByteAcc, NoLock, Preserve)
                {
                    Offset (0x0E),
                    HDRT, 8,
                    Offset (0x19),
                    SECB, 8
                }

                Device (PXSX)
                {
                    Name (_ADR, Zero)
                    OperationRegion (PCCX, PCI_Config, Zero, 0x10)
                    Field (PCCX, ByteAcc, NoLock, Preserve)
                    {
                        DVID, 32
                    }
                }
            }

            Device (RP02)
            {
                Name (_ADR, 0x001C0001)
                Device (PXSX)
                {
                    Name (_ADR, Zero)
                    OperationRegion (PCCX, PCI_Config, Zero, 0x10)
                    Field (PCCX, ByteAcc, NoLock, Preserve)
                    {
                        DVID, 32
                    }
                }
            }
        }
    }

    Method (MAIN, 0, NotSerialized)
    {
        /* Makes RP01 a multi-function bridge to bus 5 before anything below it is read. */
        \_SB.PC00.RP01.HDRT = 0x81
        \_SB.PC00.RP01.SECB = 0x05

        /* The device below RP01 is 5:0.0, which is absent: all ones. */
        If ((\_SB.PC00.RP01.PXSX.DVID != 0xFFFFFFFF))
        {
            Return (1)
        }

        /* 0:0.0 is not the function RP01's child reaches. */
        \_SB.PC00.HBRG.HVID = 0x12345678
        If ((\_SB.PC00.RP01.PXSX.DVID != 0xFFFFFFFF))
        {
            Return (2)
        }

        /* Below RP02, which is no bridge, the child stays on bus 0: it is 0:0.0. */
        If ((\_SB.PC00.RP02.PXSX.DVID != 0x12345678))
        {
            Return (3)
        }

        Return (0)
    }
}
