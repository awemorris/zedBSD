/*
 * ws159-p002: the connection resources of an I2C-HID touchpad, as the
 * Latitude 5330's \_SB.PC00.I2C1.TPD0 gives them (an I2cSerialBusV2 and a
 * GpioInt), with an SPI connection and a GPIO I/O connection that the walk
 * steps over, and a shared, edge-triggered GpioInt.  aml-host walks \TPD0._CRS; the
 * lines it must print are in connection.output.
 */
DefinitionBlock ("", "DSDT", 2, "ZEDBSD", "CONN", 1)
{
    Device (TPD0)
    {
        Name (_HID, "ZED0040")
        Name (_CRS, ResourceTemplate ()
        {
            I2cSerialBusV2 (0x002C, ControllerInitiated, 0x00061A80,
                AddressingMode7Bit, "\\_SB.PC00.I2C1",
                0x00, ResourceConsumer, , Exclusive,
                )
            GpioInt (Level, ActiveLow, ExclusiveAndWake, PullDefault, 0x0000,
                "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                )
                {   // Pin list
                    0x0147
                }
            SpiSerialBusV2 (0x0000, PolarityLow, FourWireMode, 0x08,
                ControllerInitiated, 0x00F42400, ClockPolarityLow,
                ClockPhaseFirst, "\\_SB.PC00.SPI1",
                0x00, ResourceConsumer, , Exclusive,
                )
            GpioIo (Exclusive, PullUp, 0x0000, 0x0000, IoRestrictionNone,
                "\\_SB.GPI0", 0x00, ResourceConsumer, ,
                )
                {   // Pin list
                    0x0010
                }
            GpioInt (Edge, ActiveHigh, Shared, PullNone, 0x0000,
                "\\_SB.GPI1", 0x00, ResourceConsumer, ,
                )
                {   // Pin list
                    0x0020
                }
            I2cSerialBusV2 (0x0150, ControllerInitiated, 0x000186A0,
                AddressingMode10Bit, "\\_SB.PC00.I2C0",
                0x00, ResourceConsumer, , Exclusive,
                )
        })
    }

    Method (MAIN, 0, NotSerialized)
    {
        Return (Zero)
    }
}
