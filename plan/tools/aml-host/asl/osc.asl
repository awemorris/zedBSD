/*
 * The _OSC shapes a firmware may have, as the operating system meets them
 * (ACPI 6.5 section 6.2.11): an interface that acknowledges what it is
 * asked, one that drops bits, the three refusals (failure, unrecognized
 * UUID, unrecognized revision), an Integer in place of the buffer, a
 * buffer shorter than the one passed, a query that changes state, an AML
 * exception, and a long run of Sleep.  Under a PCI Express host bridge
 * (PNP0A08): one that keeps hot plug, PME and AER to itself, one that
 * fails a request for a control bit it does not know, and one that writes
 * its own state already on the query.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds, or the number of the first check
 * that does not.  osc.evals evaluates \XEXC, whose _OSC divides by zero:
 * the evaluation must fail in both interpreters.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "OSC", 1)
{
    /* The platform-wide UUID and the PCI Express host bridge UUID. */
    Name (UPLT, ToUUID ("0811B06E-4A27-44F9-8D60-3CBBC22E7B48"))
    Name (UPCI, ToUUID ("33DB4D5B-1FF7-401C-9657-7441C03DD766"))

    /* What the well-behaved interface saw: the number of calls and the last DWORDs. */
    Name (NCAL, 0)
    Name (LST1, 0)
    Name (LST2, 0)

    /* The state the query-changing interfaces write. */
    Name (QSTA, 0)
    Name (PSTA, 0)

    /* Acknowledges every bit asked, keeping what it saw. */
    Device (GOOD)
    {
        Name (_HID, "ZED0101")

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            CreateDWordField (Arg3, 4, CDW2)
            NCAL++
            LST1 = CDW1
            LST2 = CDW2
            If (Arg0 != UPLT)
            {
                CDW1 |= 0x04
                Return (Arg3)
            }
            If (Arg1 != 1)
            {
                CDW1 |= 0x08
                Return (Arg3)
            }
            Return (Arg3)
        }
    }

    /* Keeps only bit 5 of what it is asked, and says so. */
    Device (MASK)
    {
        Name (_HID, "ZED0102")

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            CreateDWordField (Arg3, 4, CDW2)
            If (CDW2 & ~0x20)
            {
                CDW1 |= 0x10
            }
            CDW2 &= 0x20
            Return (Arg3)
        }
    }

    /* Cannot process the request. */
    Device (FAIL)
    {
        Name (_HID, "ZED0103")

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            CDW1 |= 0x02
            Return (Arg3)
        }
    }

    /* Returns an Integer. */
    Device (XINT)
    {
        Name (_HID, "ZED0104")

        Name (IVAL, 0x60)

        Method (_OSC, 4, NotSerialized)
        {
            Return (IVAL)
        }
    }

    /* Returns one DWORD where two were passed. */
    Device (XSHT)
    {
        Name (_HID, "ZED0105")

        Method (_OSC, 4, NotSerialized)
        {
            Return (Buffer (4) {0, 0, 0, 0})
        }
    }

    /* Writes its state on every call, a query too. */
    Device (QCHG)
    {
        Name (_HID, "ZED0106")

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 4, CDW2)
            QSTA = CDW2
            Return (Arg3)
        }
    }

    /* Fails with an AML exception. */
    Device (XEXC)
    {
        Name (_HID, "ZED0107")

        Name (DIVZ, 0)

        Method (_OSC, 4, NotSerialized)
        {
            Local0 = 1 / DIVZ
            Return (Arg3)
        }
    }

    /* Waits ten times 100 ms before it answers. */
    Device (SLOW)
    {
        Name (_HID, "ZED0108")

        Method (_OSC, 4, Serialized)
        {
            Local0 = 0
            While (Local0 < 10)
            {
                Sleep (100)
                Local0++
            }
            Return (Arg3)
        }
    }

    /* A host bridge that keeps hot plug, PME and AER (control bits 0, 2, 3). */
    Device (PCI0)
    {
        Name (_HID, EisaId ("PNP0A08"))
        Name (_CID, EisaId ("PNP0A03"))

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            If (Arg0 != UPCI)
            {
                CDW1 |= 0x04
                Return (Arg3)
            }
            CreateDWordField (Arg3, 8, CDW3)
            If (CDW3 & 0x0D)
            {
                CDW1 |= 0x10
            }
            CDW3 &= ~0x0D
            Return (Arg3)
        }
    }

    /* A host bridge that fails a request for a control bit it does not know (above bit 7). */
    Device (PCI1)
    {
        Name (_HID, EisaId ("PNP0A08"))

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 0, CDW1)
            CreateDWordField (Arg3, 8, CDW3)
            If (CDW3 & ~0xBF)
            {
                CDW1 |= 0x02
                CDW3 = 0
            }
            Return (Arg3)
        }
    }

    /* A host bridge that records the control asked already on the query. */
    Device (PCI2)
    {
        Name (_HID, EisaId ("PNP0A08"))

        Method (_OSC, 4, Serialized)
        {
            CreateDWordField (Arg3, 8, CDW3)
            PSTA = CDW3
            Return (Arg3)
        }
    }

    Method (MAIN, 0, Serialized)
    {
        Name (BUF, Buffer (12) {})
        CreateDWordField (BUF, 0, DW1)
        CreateDWordField (BUF, 4, DW2)
        CreateDWordField (BUF, 8, DW3)

        /* The well-behaved interface: a query, then a commit, each acknowledged. */
        Local0 = \GOOD._OSC (UPLT, 1, 2, Buffer (8) {1, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {1, 0, 0, 0, 0x60, 0, 0, 0}) { Return (1) }
        Local0 = \GOOD._OSC (UPLT, 1, 2, Buffer (8) {0, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {0, 0, 0, 0, 0x60, 0, 0, 0}) { Return (2) }
        If (NCAL != 2) { Return (3) }
        If (LST1 != 0) { Return (4) }
        If (LST2 != 0x60) { Return (5) }

        /* The same interface refuses another UUID and another revision. */
        Local0 = \GOOD._OSC (UPCI, 1, 2, Buffer (8) {0, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {4, 0, 0, 0, 0x60, 0, 0, 0}) { Return (6) }
        Local0 = \GOOD._OSC (UPLT, 2, 2, Buffer (8) {0, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {8, 0, 0, 0, 0x60, 0, 0, 0}) { Return (7) }

        /* Bits dropped: the masked bit and only bit 5 back. */
        Local0 = \MASK._OSC (UPLT, 1, 2, Buffer (8) {1, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {0x11, 0, 0, 0, 0x20, 0, 0, 0}) { Return (8) }

        /* A failure. */
        Local0 = \FAIL._OSC (UPLT, 1, 2, Buffer (8) {0, 0, 0, 0, 0x60, 0, 0, 0})
        If (Local0 != Buffer (8) {2, 0, 0, 0, 0x60, 0, 0, 0}) { Return (9) }

        /* An Integer and a short buffer. */
        Local0 = \XINT._OSC (UPLT, 1, 2, Buffer (8) {})
        If (ObjectType (Local0) != 1) { Return (10) }
        Local0 = \XSHT._OSC (UPLT, 1, 2, Buffer (8) {})
        If (ObjectType (Local0) != 3) { Return (11) }
        If (SizeOf (Local0) != 4) { Return (12) }

        /* A query that writes the state. */
        Local0 = \QCHG._OSC (UPLT, 1, 2, Buffer (8) {1, 0, 0, 0, 0x40, 0, 0, 0})
        If (QSTA != 0x40) { Return (13) }

        /* Ten sleeps of 100 ms, then the buffer back. */
        Local0 = \SLOW._OSC (UPLT, 1, 2, Buffer (8) {0, 0, 0, 0, 0x20, 0, 0, 0})
        If (Local0 != Buffer (8) {0, 0, 0, 0, 0x20, 0, 0, 0}) { Return (14) }

        /* The host bridge keeps hot plug, PME and AER: 0x3D asked, 0x30 back, masked. */
        DW1 = 1
        DW2 = 0x11
        DW3 = 0x3D
        Local0 = \PCI0._OSC (UPCI, 1, 3, BUF)
        If (Local0 != Buffer (12) {0x11, 0, 0, 0, 0x11, 0, 0, 0, 0x30, 0, 0, 0}) { Return (15) }

        /* Only the PCI Express Capability and LTR asked: granted as asked. */
        DW1 = 0
        DW2 = 0x11
        DW3 = 0x30
        Local0 = \PCI0._OSC (UPCI, 1, 3, BUF)
        If (Local0 != Buffer (12) {0, 0, 0, 0, 0x11, 0, 0, 0, 0x30, 0, 0, 0}) { Return (16) }

        /* The platform-wide UUID at a host bridge is not recognized. */
        DW1 = 1
        Local0 = \PCI0._OSC (UPLT, 1, 3, BUF)
        If (Local0 != Buffer (12) {5, 0, 0, 0, 0x11, 0, 0, 0, 0x30, 0, 0, 0}) { Return (17) }

        /* A control bit the bridge does not know fails the request and clears the control. */
        DW1 = 1
        DW2 = 0x11
        DW3 = 0x130
        Local0 = \PCI1._OSC (UPCI, 1, 3, BUF)
        If (Local0 != Buffer (12) {3, 0, 0, 0, 0x11, 0, 0, 0, 0, 0, 0, 0}) { Return (18) }

        /* A query that already records the control asked. */
        DW1 = 1
        DW2 = 0x11
        DW3 = 0x05
        Local0 = \PCI2._OSC (UPCI, 1, 3, BUF)
        If (PSTA != 0x05) { Return (19) }

        Return (0)
    }
}
