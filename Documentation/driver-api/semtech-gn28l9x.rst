.. SPDX-License-Identifier: GPL-2.0-only

========================
Semtech GN28L9x frontends
========================

The ``gn25l95`` module (``CONFIG_GN25L95_PHY``) shares the lower A2 DDMI,
alarm decoding, frontend registration and hwmon bridge between GN25L95,
GN25L98, GN28L96 and GN28L98. Multi-byte measurements are decoded MSB first,
independently of host CPU endianness.

GN28L96 support
===============

The GN28L96 implementation follows the Final Data Sheet Rev.9 (PDS-061332,
November 2022), particularly sections 3.2, 3.3, 3.9 and 4.3 through 4.12.
It advertises XG-PON, XGS-PON and 10G EPON frontend capability. The MAC,
digital PHY and board calibration must also support the selected line mode.
This driver does not derive an analog configuration from a protocol name.

The chip is instantiated at its A2 I2C address, normally ``0x51``::

  bosa_frontend: optical-frontend@51 {
      compatible = "semtech,gn28l96";
      reg = <0x51>;
      #optical-frontend-cells = <0>;
      firmware-name = "semtech/board-gn28l96-hwconfig.bin";
  };

The firmware property is used only in MCU mode. In factory EEPROM mode,
omit it: the driver verifies the chip identity, startup key and data-ready
status, then preserves the configuration loaded by the chip. Vendor tables
must be readable; if the board protects them, supply its actual password in
``semtech,password``. No password is guessed and no reset is issued.

Host calibration
----------------

MCU-mode boards require the default A2 address and their own factory-calibrated
image. No universal
laser/APD values are supplied. Install the image under ``/lib/firmware``.
The default name is ``semtech/gn28l96-hwconfig.bin``.

The image layout is byte-oriented; all register words retain their original
MSB-first representation:

=================  ======  ==============================================
Offset             Length  Content
=================  ======  ==============================================
0x000              256     A0 identification (retained, not programmed)
0x100              256     A2 lower and upper table 0
0x200              128     Table 0x80: Tx/Rx/APD controls
0x280              128     Table 0x81: digital controls and calibration
0x300              128     Table 0x82: bias/modulation LUTs
0x380              128     Table 0x83: APCSET/APD LUTs
0x400 (optional)   128     Table 0x84: extinction ratio/eye shaping LUTs
0x480 (optional)   128     Table 0x85: Tx power/dark current LUTs
0x500 (optional)   128     Table 0x86: receiver equalization LUT
=================  ======  ==============================================

Only 1024-byte and 1408-byte images are accepted. The longer format is
required when table 0x81 register 0xb6 enables a LUT in tables 0x84..0x86.
GN25L95/GN25L98 images must not be renamed for use on GN28L96.

The driver inhibits EEPROM updates and holds software TX_DISABLE while
programming the calibration. Writable configuration is programmed without
overwriting live measurements, reserved byte locations, passwords, access
permissions or I2C addressing. DAC words use aligned two-byte writes.
The startup key is committed after calibration/LUT verification. TX remains
disabled until the protocol consumer explicitly enables or rearms it.
An image which inhibits software TX_DISABLE is rejected.

Factory calibration may use inverted TX_DISABLE polarity. The driver reads
the reporting mode and polarity from table 0x80 register 0x80 so that the
generic frontend reports the effective transmitter state. TX_FAULT pad
polarity and receiver LOS/SD polarity are also normalized using registers
0x9a and 0xed, respectively.

GN28L98 limitations
===================

GN28L98 is an experimental, read-only SFF diagnostics variant. It shares
lower-page temperature, voltage, bias, optical power, alarm and status
access on boards whose boot firmware or EEPROM has already initialized a
compatible SFF interface. It advertises XGS-PON capability only.

The GN28L98 register map was not provided with the GN28L96 datasheet and
was not found in the supplied SDK. Its vendor table layout, password
mechanism, startup key and host calibration must not be inferred from the
GN28L96 or GN25L98. This variant does not select vendor pages, reset the
chip, request calibration firmware or implement transmitter control.
The SFF diagnostics assumption still requires verification on hardware.

An unconfigured GN28L98 cannot be brought up by this driver. Full MCU-mode
support requires its datasheet or a confirmed vendor initialization trace
and board-specific calibration data.

Diagnostics
===========

The generic provider appears under ``/sys/class/optical_frontend`` and
exports telemetry through its ``hwmon`` link. Debugfs provides ``diag`` at
``/sys/kernel/debug/<I2C-device>/diag``. GN28L96 also exposes ``tx_rearm``.
Physical burst-enable and TX_DISABLE GPIOs remain owned by the digital PHY
consumer, as described in :doc:`optical-frontend`.
