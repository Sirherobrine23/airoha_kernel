.. SPDX-License-Identifier: GPL-2.0-only

EcoNet MIPS CPU frequency reporting
==================================

The ``econet`` CPUFreq driver reports the CPU frequency on EN751221,
EN751627, EN7528 and EN7580 SoCs. It exposes a single frequency shared by all
cores and VPEs. It does not program the CPU PLL, change bus or DDR clocks,
or implement runtime frequency scaling or thermal throttling.

Enable ``CONFIG_CPU_FREQ``, ``CONFIG_COMMON_CLK_EN7523`` and
``CONFIG_ECONET_CPUFREQ``. The latter can be built in or built as the
``econet-cpufreq`` module. The system clock controller creates its platform
device after registering the clocks. The CPU nodes must reference the
controller's CPU clock, as provided by the EcoNet SoC DTSI files.

Frequency sources
-----------------

On EN751221 and EN7580, the CPU and bus clocks use uncached ``recalc_rate``
callbacks to read the programmed PLL configuration. The bus clock is the CPU
clock divided by four on EN751221, or by five on EN7580. Both clocks read the
hardware, so reading the bus clock alone does not depend on a prior CPU clock
read. These are configured nominal rates, not cycle-counter measurements.

EN751221 recognizes the following complete register profiles, with ``0x12``
at ``0x1fa20144`` and ``0x406`` at ``0x1fa201a8``:

========== ============ ============ ================================
CPU        0x1fa2019c   0x1fa201ac   Source
========== ============ ============ ================================
700 MHz    0x05101b08   0x02800000   Vendor bootloader
800 MHz    0x05101f08   0x03000000   Interpolated, exercised on EN7526G
900 MHz    0x05102308   0x03800000   Vendor bootloader
========== ============ ============ ================================

No other rates are extrapolated from these undocumented fields. In
particular, ``0x05100c08`` / ``0x00a00000`` is not reported as 325 MHz:
the benchmark result did not establish that frequency. EN7526C/EN7522 use
a different PLL layout and retain boot-metadata reporting even when they
share the EN751221 clock-controller compatible.

On EN7580, ``do_cpufreq`` in the SDK bootram programs bits 30:24 of
``0x1fa202ac``. A PCW of 104 corresponds to 1300 MHz, with each decrement
of two corresponding to 25 MHz. The driver recognizes the SDK's even PCW
values from 26 through 104 (325 through 1300 MHz), provided the rest of the
register is zero. The nominal CPU rate is ``PCW * 12500000`` Hz. This
interpretation is specific to the EN7580 path and is not used on EN751221.

At clock-controller probe, the driver saves the PLL register state and
the bootloader CPU rate. An otherwise unrecognized profile can retain
that bootloader rate only while the saved PLL register state is unchanged.
This preserves boot configurations not covered by the decoders, including
the EN751221 default/940 MHz boot path. A different unrecognized state
returns zero from the clock provider; CPUFreq displays ``<unknown>`` in
``cpuinfo_cur_freq``. Restore a recognized profile to resume live reporting.

EN751627 and EN7528 retain the fixed bootloader rate. All platforms still
require nonzero bootloader metadata to initialize the clock controller.

Bootloader metadata
~~~~~~~~~~~~~~~~~~~

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
There is no guessed fallback frequency or CP0 measurement. A metadata-only
rate remains valid only while the boot configuration is unchanged.

The field contains integer MHz. Vendor boot paths that save ``CPU >> 2``
truncate CPU rates that are not multiples of 4 MHz; the reconstructed CPU
rate then has the same precision limit. The driver does not infer the lost
bits or substitute a guessed PLL rate.

For example, ``0x000e1100`` on EN751221 contains a bus frequency of 225 MHz
and corresponds to a CPU frequency of 900 MHz.

The external 200 MHz high-precision timer is independent of this reported
CPU clock. Its frequency and the MIPS timer setup are not modified.

User interface
--------------

Read the shared policy with::

    cat /sys/devices/system/cpu/cpufreq/policy0/scaling_driver
    cat /sys/devices/system/cpu/cpufreq/policy0/scaling_available_frequencies
    cat /sys/devices/system/cpu/cpufreq/policy0/cpuinfo_cur_freq
    cat /sys/devices/system/cpu/cpufreq/policy0/related_cpus

For the 900 MHz example, the driver name is ``econet``. On the live-reporting
platforms, ``cpuinfo_cur_freq`` follows recognized changes to the PLL
registers after boot. Read it after a completed register sequence, not while
another process is programming the PLL. Register readback does not confirm
PLL lock or establish the active mux source.

The sole policy-table entry is the rate captured when CPUFreq registers;
minimum and maximum frequencies remain identical. This table is not a list
of supported transitions. Automatic dynamic governors are disabled, and
the driver does not restore a rate changed outside CPUFreq. Such a request
returns ``-EOPNOTSUPP``. ``scaling_cur_freq`` follows the policy's last
observed rate; use ``cpuinfo_cur_freq`` for an on-demand PLL read.

The Common Clock Framework also exposes the CPU and bus clocks in
``/sys/kernel/debug/clk/clk_summary`` when clock debugging is available.
The policy number may differ if CPU0 is offline when the policy is created.

This change adds no PLL writes, transition notifiers, watchdog retiming or
delay-loop adjustment. Direct register writes bypass clock and CPUFreq
transition handling; observing their result does not make such writes a
kernel-managed frequency transition.
