EN7580 XSI PCS bring-up
======================

Scope
-----

This is an experimental port of the Ethernet XSI lane, recovered from the
Quantum C6500XK vendor firmware's unstripped ``xsi_phy.ko``. It is separate
from the optical PON PHY and from the AN7581/AN7583 PCS implementations.

The provider supports out-of-band 10GBASE-R, 5GBASE-R, 2500BASE-X and
1000BASE-X. SGMII negotiation, USXGMII, interrupts and the vendor eye-scan/TDC
recovery paths are not implemented. In particular, 1000BASE-X must not be
substituted for an AQR113C firmware profile that selects SGMII at 1 Gbit/s.
The AQR113C firmware upload remains the responsibility of its PHY driver.

Register map
------------

=================== ============ =========================================
Block               Physical base Purpose
=================== ============ =========================================
XSI MAC             0x1fa04000   Traffic gating, pause, counters
XSI PMA             0x1faf6000   PLL, analog settings, RX calibration
XFI PCS             0x1faf7900   RX/TX reset and readiness at offset 0x30
HSGMII PCS          0x1faf8108   RX/TX reset; readiness at 0x1faf821c
NP SCU              0x1fb00000   Lane selection, reset and clock controls
Chip SCU            0x1fa20000   XSI enable at offset 0x104
=================== ============ =========================================

Recovery evidence
-----------------

The init table is the ``xsi_init(mode)`` path (235 operations). The start
and calibration table is ``xsi_plug_reset(1, mode)`` (174 operations).
Vendor modes 0, 1, 4 and 5 select 10G, 5G, 2.5G and 1G respectively.
There are 21 mode-dependent values across these 409 operations. The rest
share the same register, mask and value. Repeated writes are retained and
forced through regmap, including calibration/reset strobes. The external
RX/TX PCS reset order is preserved separately around the start table.

The initial device controls come from ``en7580_xfi_phy_dev_init``. The
NP SCU reset at offset 0x830, bit 8 comes from both vendor PHY reset
functions. It is accessed through the shared syscon regmap since this
repository does not yet match ``econet,en7580-scu`` in the clock driver.
That separate missing clock/reset provider is not repaired by this port.

The status checks reproduce ``en7580_xfi_phy_get_api_dispatch`` (bits 12
and 0 at 0x1faf7930) and ``en7580_hsgmii_phy_get_api_dispatch`` (low nibble
0xa at 0x1faf821c). No AN7581 register layout is assumed.

The firmware boot log reports successful XFI initialization and later
``LineRate 2500 PhyMode 4``. This demonstrates the vendor path, not the
operation of this new driver on hardware.

Validation and testing
----------------------

All 409 register/mask/value and delay operations for each of the four
modes were compared against an independent Unicorn MIPS32 little-endian
emulation of the original ELF code. The traces matched exactly.

The PCS object was compiled against this kernel with an x86_64 compile-test
configuration. The Ethernet object also compiled with its pre-existing
64-bit format warning at the GPON OAM descriptor log demoted using
``KCFLAGS=-Wno-error=format``. These checks do not replace a MIPS build,
module link or hardware test. Checkpatch has no errors or warnings in the new PCS source and sequence
header. The SoC DTS compiled with dtc; the existing switch node emits an
unnecessary address/size-cells warning.

Enable ``CONFIG_PCS_AIROHA_EN7580`` and enable the ``xsi_pcs`` node in the
board DTS. Add ``pcs-handle = <&xsi_pcs>`` to the MAC actually routed to the
external Ethernet PHY, together with its ``phy-handle`` and verified
``phy-mode``. Do not use ``managed = "in-band-status"`` with this port.
The SoC node is disabled by default, and board wiring is intentionally not
inferred from the generic SoC DTS.

Confirm that the external PHY has a valid firmware image before testing.
Start with the vendor-supported 10GBASE-R configuration, then test 5G and
2.5G copper links while checking the PHY's host-interface selection. Record
PCS status, negotiated interface, carrier and bidirectional traffic. Test
unplug/replug and interface down/up. RX recovery beyond the normal initial
calibration has not yet been ported.

This patch provides a PCS/PMA bring-up candidate and phylink discovery.
It does not establish the complete EN7580 XSI FE/QDMA forwarding path,
source-port mapping or simultaneous operation with the xPON MAC. Those
remain separate work before declaring the Ethernet WAN functional.
