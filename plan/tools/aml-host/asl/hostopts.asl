/*
 * The harness's options for following a firmware call: --set stores a
 * value into a name and into a field unit of system memory before the
 * calls, --memory-file places support/dynamic2.aml in system memory where
 * \TBLD's Load finds it, --trace prints the writes of each call (system
 * memory, system I/O, PCI configuration space), the table loaded, the
 * Debug and Notify log lines and the time slept, and --diff prints the
 * names added and the values changed.
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when the values --set stored are there.  hostopts.evals
 * then runs \TBLD under the trace; the lines the harness must print are in
 * hostopts.output.  acpiexec has none of these options.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "HOSTOPT", 1)
{
    External (\DYN2, IntObj)

    /* A value --set gives, and a field of system memory --set writes. */
    Name (SETV, 0)
    OperationRegion (NVSR, SystemMemory, 0x00500000, 0x10)
    Field (NVSR, ByteAcc, NoLock, Preserve)
    {
        NVS0, 8,
        NVS1, 32
    }

    /* The table --memory-file places at 0x00600000, which \TBLD loads. */
    OperationRegion (TBLR, SystemMemory, 0x00600000, 0x2D)
    Name (HNDL, 0)

    /* A port and a PCI configuration register \TBLD writes. */
    OperationRegion (CMIO, SystemIO, 0x72, 2)
    Field (CMIO, ByteAcc, NoLock, Preserve)
    {
        IDX0, 8,
        DAT0, 8
    }

    Device (PCI0)
    {
        Name (_HID, EisaId ("PNP0A08"))
        Name (_ADR, 0)

        Device (DEV1)
        {
            Name (_ADR, 0x000D0002)
            OperationRegion (CFG0, PCI_Config, 0, 0x100)
            Field (CFG0, DWordAcc, NoLock, Preserve)
            {
                Offset (0xFC),
                FPWR, 32
            }
        }
    }

    Method (MAIN, 0, NotSerialized)
    {
        If (SETV != 0x1234) { Return (1) }
        If (NVS0 != 0x5A) { Return (2) }
        Return (0)
    }

    /* Loads the table, writes the port pair, the PCI register and the field, notifies and sleeps. */
    Method (TBLD, 0, Serialized)
    {
        Load (TBLR, HNDL)
        IDX0 = 0xF2
        DAT0 = 0x01
        \PCI0.DEV1.FPWR = 0x80000001
        NVS1 = 0x11223344
        Debug = "TBLD done"
        Notify (\PCI0.DEV1, 0x80)
        Sleep (5)
        Return (\DYN2)
    }
}
