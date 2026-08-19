.. SPDX-License-Identifier: GPL-2.0-only

==========================
Optical frontend subsystem
==========================

The optical frontend subsystem represents analog optical components which sit
between a protocol MAC/digital PHY and the optical medium.  Typical examples
are burst-mode laser drivers, limiting amplifiers and APD controllers used by
PON equipment.

An optical frontend is deliberately not a ``struct phy`` and not an Ethernet
PHY.  The generic PHY framework continues to describe the digital/serial PHY,
while ``struct optical_frontend`` describes the analog optical device.  A MAC,
PHY, protocol-management implementation such as OMCI, hwmon and diagnostic
interfaces may all hold references to the same frontend object.

Provider and consumer model
===========================

A provider registers one frontend with::

  frontend = devm_optical_frontend_register(dev, &desc, &ops, priv);

Firmware consumers refer to the provider using ``optical-frontends`` and
``optical-frontend-names``::

  optical-frontends = <&bosa_frontend>;
  optical-frontend-names = "pon";

and obtain it with::

  frontend = devm_optical_frontend_get_optional(dev, "pon");

The first version of the binding supports providers with
``#optical-frontend-cells = <0>``.  The core creates a device link from each
consumer to the physical provider and reference-counts the frontend device.
Consequently multiple consumers refer to one provider state and one telemetry
cache rather than independently accessing the same I2C device.

Ownership
=========

Sharing a frontend does not imply that every consumer should control it.  The
protocol MAC normally owns operational transitions such as selecting the PON
mode or rearming a transmitter latch.  Read-only consumers such as OMCI,
hwmon or diagnostics should use ``optical_frontend_get_state()`` and
``optical_frontend_get_telemetry()``.

The core serializes provider callbacks.  Providers which also have periodic or
interrupt-driven hardware access should protect that access with their private
lock and avoid calling a core operation while holding the private lock if that
operation can enter the provider again.

Normalized telemetry
====================

``struct optical_frontend_telemetry`` uses kernel-wide normalized units:

* temperature: milli-degrees Celsius
* supply voltage: microvolts
* laser bias: microamps
* optical Tx/Rx power: nanowatts

Providers translate their native register representation into these units.
Protocol layers such as OMCI then perform only protocol-specific encoding and
never need to know the frontend vendor or register layout.

Operating mode
==============

``struct optical_frontend_mode`` identifies the line protocol, nominal Tx/Rx
bit rates and burst/continuous operation.  Consumers call
``optical_frontend_set_mode()``; a provider may implement ``->set_mode()`` when
hardware programming is required.  Even when no callback is required the core
retains the selected mode for shared consumers and compatibility interfaces.

SFF-8472 compatibility
======================

``CONFIG_OPTICAL_FRONTEND_SFP_COMPAT`` optionally creates a virtual I2C adapter
for a provider DT child named ``i2c-sfp``.  It exports generated A0 identity and
A2 DDMI pages for existing SFP tooling.  This is a compatibility interface;
fixed BOSA hardware should be connected to its MAC through
``optical-frontends`` rather than by pretending that the soldered frontend is
a pluggable SFP module.
