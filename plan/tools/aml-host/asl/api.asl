/*
 * WS049 AML test (ws049-p017): the public interface drivers use beyond
 * evaluation -- a resource template walked by drv_acpi_resources_walk(),
 * work run with an AML mutex held by drv_acpi_run_locked(), a notification
 * handler that removes itself with drv_acpi_notify_remove(), and a package
 * a driver builds with drv_acpi_object_package_new() and _set().
 * Copyright (C) 2026 Awe Morris; SPDX-License-Identifier: Zlib
 *
 * MAIN returns 0 when every check holds.  It notifies \DEV2 twice; the
 * handler aml-host installs (api.args) removes itself at the first, so it
 * runs once.  aml-host then walks \RES0._CRS, runs \TAKE (which acquires
 * \MTX0 again) with \MTX0 held, and calls \PKG with its package; the lines
 * it must print are in api.output.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "API", 1)
{
    Mutex (MTX0, 0)

    Device (RES0)
    {
        Name (_HID, "ZED0030")
        Name (_CRS, ResourceTemplate ()
        {
            IO (Decode16, 0x0062, 0x0062, 0x01, 0x01)
            FixedIO (0x0066, 0x01)
            IRQNoFlags () {1, 12}
            IRQ (Level, ActiveLow, Shared) {9}
            DMA (Compatibility, NotBusMaster, Transfer8) {2}
            Memory24 (ReadWrite, 0x0100, 0x010F, 0x0001, 0x0010)
            Memory32 (ReadOnly, 0xFED00000, 0xFED003FF, 0x00000001, 0x00000400)
            Memory32Fixed (ReadWrite, 0xFE000000, 0x00001000)
            WordIO (ResourceProducer, MinFixed, MaxFixed, PosDecode, EntireRange,
                0x0000, 0x0000, 0x0CF7, 0x0000, 0x0CF8)
            WordBusNumber (ResourceProducer, MinFixed, MaxFixed, PosDecode,
                0x0000, 0x0000, 0x00FF, 0x0000, 0x0100)
            DWordMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                0x00000000, 0x80000000, 0x8FFFFFFF, 0x00000000, 0x10000000)
            QWordMemory (ResourceProducer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadOnly,
                0x0000000000000000, 0x0000000400000000, 0x00000004FFFFFFFF,
                0x0000000000000000, 0x0000000100000000)
            ExtendedMemory (ResourceConsumer, PosDecode, MinFixed, MaxFixed, Cacheable, ReadWrite,
                0x0000000000000000, 0x0000000500000000, 0x0000000500000FFF,
                0x0000000000000000, 0x0000000000001000, 0x0000000000000000)
            Interrupt (ResourceConsumer, Edge, ActiveHigh, Exclusive) {0x20, 0x21}
        })
    }

    Device (DEV2)
    {
        Name (_HID, "ZED0031")
    }

    /* Acquires \MTX0 again: inside drv_acpi_run_locked() the entry already holds it. */
    Method (TAKE, 0, Serialized)
    {
        Local0 = Acquire (MTX0, 0)
        If (Local0 != 0)
        {
            Return (0xDEAD)
        }

        Release (MTX0)
        Return (0x55)
    }

    /* Checks the package a driver built: Integer 0x1234 and String "zed". */
    Method (PKG, 1)
    {
        If (ObjectType (Arg0) != 4)
        {
            Return (1)
        }

        If (SizeOf (Arg0) != 2)
        {
            Return (2)
        }

        If (DerefOf (Arg0 [0]) != 0x1234)
        {
            Return (3)
        }

        If (DerefOf (Arg0 [1]) != "zed")
        {
            Return (4)
        }

        Return (0)
    }

    Method (MAIN, 0)
    {
        Notify (DEV2, 0x80)
        Notify (DEV2, 0x81)
        Return (0)
    }
}
