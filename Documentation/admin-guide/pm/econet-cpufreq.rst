.. SPDX-License-Identifier: GPL-2.0-only

EcoNet MIPS CPU frequency reporting
==================================

The ``econet`` CPUFreq driver reports the boot CPU frequency on EN751221,
EN751627, EN7528 and EN7580 SoCs. It exposes a single frequency shared by all
cores and VPEs. It does not program the CPU PLL, change bus or DDR clocks,
or implement runtime frequency scaling or thermal throttling.

Enable ``CONFIG_CPU_FREQ``, ``CONFIG_COMMON_CLK_EN7523`` and
``CONFIG_ECONET_CPUFREQ``. The latter can be built in or built as the
``econet-cpufreq`` module. The system clock controller creates its platform
device after registering the clocks. The CPU nodes must reference the
controller's CPU clock, as provided by the EcoNet SoC DTSI files.

Frequency source
----------------

The system clock controller reads the bus frequency, in MHz, saved by the
bootloader at physical address ``0x1fb00284``. The layout and multiplier are
SoC-specific:

========== =================== =========================
SoC        Bus frequency field CPU frequency calculation
========== =================== =========================
EN751221   bits 21:12          bus frequency * 4
EN751627   bits 21:12          bus frequency * 4
EN7528     bits 19:10          bus frequency * 4
EN7580     bits 19:10          bus frequency * 5
========== =================== =========================

These layouts follow the vendor DRAMC boot paths. In particular, the
preserved EN751627 calibration binary inserts ten bits at bit 12; a header
declaring a shift of 20 does not describe that binary's clock field.

A zero bus frequency is rejected by the clock controller. Other values
depend on the bootloader having populated the expected field correctly.
There is no guessed fallback frequency and no live PLL or CP0 measurement.
The reported frequency remains valid only while the boot configuration is
unchanged. This interface must not be treated as a frequency monitor for
changes made by firmware or direct register writes after boot.

The field contains integer MHz. Vendor boot paths that save ``CPU >> 2``
truncate CPU rates that are not multiples of 4 MHz; the reconstructed CPU
rate then has the same precision limit. The driver does not infer the lost
bits or substitute a guessed PLL rate.

For example, ``0x000e1100`` on EN751221 contains a bus frequency of 225 MHz
and corresponds to a CPU frequency of 900 MHz. A matching observed PLL
configuration is ``0x05102308`` at ``0x1fa2019c`` and ``0x03800000`` at
``0x1fa201ac``, with ``0x00000406`` at ``0x1fa201a8``. These values are
reference observations, not a PLL decoder implemented by this driver.

The external 200 MHz high-precision timer is independent of this reported
CPU clock. Its frequency and the MIPS timer setup are not modified.

User interface
--------------

Read the shared policy with::

    cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver
    cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies
    cat /sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq
    cat /sys/devices/system/cpu/cpufreq/policy0/related_cpus

For the 900 MHz example, the driver name is ``econet`` and the sole
frequency is ``900000`` kHz. Minimum and maximum frequencies are identical.
Automatic dynamic governors are disabled for this reporting-only driver.
Selecting the sole frequency performs no hardware write.

The Common Clock Framework also exposes the CPU clock in
``/sys/kernel/debug/clk/clk_summary`` when clock debugging is available.
The policy number may differ if CPU0 is offline when the policy is created.
