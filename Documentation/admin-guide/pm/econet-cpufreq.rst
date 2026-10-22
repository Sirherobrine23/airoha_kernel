.. SPDX-License-Identifier: GPL-2.0-only

EcoNet MIPS CPU frequency control and reporting
=============================================

The ``econet`` CPUFreq driver reports the CPU frequency on EN751221,
EN751627, EN7528 and EN7580 SoCs. It exposes a single frequency shared by all
cores and VPEs. EN751221-family and EN7528-family configurations support
experimental explicit profile changes; other devices retain reporting-only
operation. DDR settings and the external timer are not changed.

Enable ``CONFIG_CPU_FREQ``, ``CONFIG_COMMON_CLK_EN7523`` and
``CONFIG_ECONET_CPUFREQ``. The latter can be built in or built as the
``econet-cpufreq`` module. The system clock controller creates its platform
device after registering the clocks. The CPU nodes must reference the
controller's CPU clock, as provided by the EcoNet SoC DTSI files.

Frequency sources
-----------------

On EN751221, EN7528 and EN7580, the CPU clock uses an uncached
``recalc_rate`` callback to read the programmed PLL configuration. The bus
clock is the CPU clock divided by four on EN751221 and EN7528, or by five
on EN7580. BUS derives its rate
from the CPU parent, allowing CCF to predict and notify bus changes before
programming the PLL. CPU readback refreshes that parent. Direct MMIO changes
bypass this mechanism; when inspecting them, read the CPU rate first. These
are configured nominal rates, not cycle-counter measurements.

EN751221 recognizes the following complete register profiles, with ``0x12``
at ``0x1fa20144`` and ``0x406`` at ``0x1fa201a8``:

========== ============ ============ ================================
CPU        0x1fa2019c   0x1fa201ac   Source
========== ============ ============ ================================
400 MHz    0x05100c08   0x00a00000   Measured on EN7526G with the PMU
700 MHz    0x05101b08   0x02800000   Vendor bootloader
800 MHz    0x05101f08   0x03000000   Interpolated, exercised on EN7526G
900 MHz    0x05102308   0x03800000   Vendor bootloader
1000 MHz   0x05102708   0x04000000   Short PMU windows on EN7526G
========== ============ ============ ================================

The 400 MHz pair was initially extrapolated as 325 MHz, but short PMU
measurements established approximately 400.008 MHz between two 900 MHz
reference measurements. All five complete pairs are recognized throughout
the EN751221 family; no other rates are inferred automatically from these
undocumented fields. Measurements on EN7526G do not establish stability
on other family variants.
EN7526C/EN7522 use
a different PLL layout and retain boot-metadata reporting even when they
share the EN751221 clock-controller compatible.

EN7528 uses a different layout: ``0x1fa201a0`` for programming setup,
``0x1fa201ec`` for configuration, ``0x1fa201f8`` for apply control, and
``0x1fa201fc`` for the parameter. An idle recognized profile requires
setup zero and control ``0x406``. The following 28 complete pairs are
recognized without a separate characterization option:

========== ============ ============
CPU        0x1fa201ec   0x1fa201fc
========== ============ ============
325  MHz   0x05200c08   0x02400000
350  MHz   0x05200d08   0x02800000
375  MHz   0x05200e08   0x02c00000
400  MHz   0x05200f08   0x03000000
425  MHz   0x05201008   0x03400000
450  MHz   0x05201108   0x03800000
475  MHz   0x05201208   0x03c00000
500  MHz   0x05201308   0x04000000
525  MHz   0x05101408   0x01a00000
550  MHz   0x05101508   0x01c00000
575  MHz   0x05101608   0x01e00000
600  MHz   0x05101708   0x02000000
625  MHz   0x05101808   0x02200000
650  MHz   0x05101908   0x02400000
675  MHz   0x05101a08   0x02600000
700  MHz   0x05101b08   0x02800000
725  MHz   0x05101c08   0x02a00000
750  MHz   0x05101d08   0x02c00000
775  MHz   0x05101e08   0x02e00000
800  MHz   0x05101f08   0x03000000
825  MHz   0x05102008   0x03200000
850  MHz   0x05102108   0x03400000
875  MHz   0x05102208   0x03600000
900  MHz   0x05102308   0x03800000
925  MHz   0x05102408   0x03a00000
950  MHz   0x05102508   0x03c00000
975  MHz   0x05102608   0x03e00000
1000 MHz   0x05102708   0x04000000
========== ============ ============

All pairs were exercised in short pinned PMU windows on one EN7528HU,
including the transition from 525 to 500 MHz and restoration to 900 MHz.
The measured endpoints were approximately 325.013 and 1000.039 MHz, with
matching workload checksums. This establishes short-window frequency
behavior on that unit, not prolonged stability or silicon operating limits.

Below 525 MHz the pairs use a divide-by-two interpretation anchored by the
SDK's 450 MHz pair. The 325 MHz pair is ``0x05200c08`` / ``0x02400000``;
it differs from the EN751221 pair that measured 400 MHz. The measurements
support this divider interpretation but do not document a general PLL
formula. Only explicitly listed complete pairs are accepted.

The SDK's 900 MHz path only saves boot metadata. Runtime restoration uses
the observed boot pair, not a documented SDK 900 MHz transition. The SDK's
separate 225 MHz profile is not exposed by this driver.

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

EN751627 retains the fixed bootloader rate. All platforms still require
nonzero bootloader metadata to initialize the clock controller.

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

On reporting-only devices, the sole policy-table entry remains the initial
rate, and minimum and maximum frequencies are identical. Such devices cannot
restore a rate changed outside CPUFreq (``-EOPNOTSUPP``). On all platforms,
use ``cpuinfo_cur_freq`` for an on-demand PLL read.

The Common Clock Framework also exposes the CPU and bus clocks in
``/sys/kernel/debug/clk/clk_summary`` when clock debugging is available.
The policy number may differ if CPU0 is offline when the policy is created.

EN751221-family profile control
------------------------------

CPU control requires the EN751221 family, a saved 900 MHz boot rate, and a
recognized initial PLL profile. No EN7526G-only variant gate is applied.
The clock controller exposes 400, 700, 800, 900 and 1000 MHz. These profiles
have short test evidence on EN7526G; enabling them on another family
variant does not establish its operating limits or stability. CPUFreq starts
with a 400 MHz floor and 900 MHz ceiling;
the 1 GHz entry is marked as software boost and disabled by default. This
policy floor is not a claim that 400 MHz is the silicon's minimum frequency.

Enable ``CONFIG_CPU_FREQ_GOV_USERSPACE`` (built in or as a module), then use::

    policy=/sys/devices/system/cpu/cpufreq/policy0
    cat "$policy/scaling_available_frequencies"
    echo userspace > "$policy/scaling_governor"
    echo 400000 > "$policy/scaling_setspeed"
    cat "$policy/cpuinfo_cur_freq"
    echo 900000 > "$policy/scaling_setspeed"

Frequencies are in kHz. The 400 MHz floor applies to supported clock API
requests and CPUFreq selection; root can still bypass both with ``devmem``.
Automatic dynamic governors remain disabled. No voltage adjustment or
thermal cooling device is introduced.

The clock provider runs the tested five-write sequence once inside
``stop_machine()``, with the other VPEs quiesced. It checks complete register
readback and attempts to restore the old profile on a mismatch. CPUFreq
verifies the resulting live rate even if CCF failed to propagate a provider
error. An unknown starting PLL state is rejected without writes. No PLL lock
bit or glitchless-switch procedure is documented: this remains experimental
support based on the observed EN7526G transitions, not a hardware guarantee.
``stop_machine()`` does not stop peripheral DMA.

MIPS transition notifiers update the delay loops for the shared CPU policy.
The Airoha watchdog listens for bus-clock changes, preserves the remaining
countdown and keeps the configured timeout for future pings. Before a clock
increase it preloads for the faster rate so the watchdog does not expire
early. An unrepresentable timeout or remaining counter vetoes the change.
A concurrent watchdog operation obtains the clock before its own mutex and
retries if a notification changed the rate generation, avoiding lock-order
inversion. If a failed provider change caused CCF to omit POST, the next
watchdog operation reconciles the conservative PRE rate with the actual
clock. In that failure case the grace period can be temporarily extended.

Do not mix direct PLL writes with CPUFreq control. MMIO writes bypass the
transition notifications, watchdog accounting and MIPS delay-loop updates.

Experimental 1 GHz test
-----------------------

The 1 GHz pair ``0x05102708`` / ``0x04000000`` has short PMU evidence
on EN7526G and EN7528HU. EN7528 also exposes 925, 950 and 975 MHz as
software boost entries. These observations do not establish prolonged
stability or validation of other EN751221 variants. CPUFreq readback only
recognizes a complete register pair; it does not measure CPU cycles or
confirm PLL lock.

This is an experimental overclock, not a documented hardware turbo mode.
No supply-voltage change is made. If CPU/4 remains the bus relationship,
the bus rises from 225 to 250 MHz; bus peripherals and DMA are exposed to
that change. The watchdog notifier remains active. There is no guarantee
of stability or recovery from a hung PLL. Use a serial console and a
recoverable test image.

After boot, the global ``/sys/devices/system/cpu/cpufreq/boost`` switch
reads zero. Rates above 900 MHz appear in ``scaling_boost_frequencies``.
Select the userspace governor and 900 MHz before enabling boost::

    policy=/sys/devices/system/cpu/cpufreq/policy0
    echo userspace > "$policy/scaling_governor"
    echo 900000 > "$policy/scaling_setspeed"
    cat "$policy/scaling_boost_frequencies"
    echo 1 > /sys/devices/system/cpu/cpufreq/boost
    echo 1000000 > "$policy/scaling_max_freq"
    echo 1000000 > "$policy/scaling_setspeed"

Measure a short pinned PMU window as in the previous tests. Restore with::

    echo 900000 > "$policy/scaling_setspeed"
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    echo 900000 > "$policy/scaling_max_freq"

Enabling boost while a performance governor is active can select 1 GHz
immediately. Do not enable it from boot scripts. The clock-provider API
knows the candidate pair even while CPUFreq boost is disabled; the boost
switch controls CPUFreq requests, not direct kernel clock API callers or
MMIO writes. EN7580 and all other reporting-only configurations retain
their previous behavior.

EN7528-family profile control
-----------------------------

The observed identification is HIR ``0x000b0000`` (EN7528 family), PDIDR
``0x00000001``, and saved parameters ``0x01038500``. The package field is
zero, identifying EN7528HU through the bootloader SCU metadata; zero eFuse
registers are not used as an EN7526G identification. Bits 19:10 contain
225 MHz, corresponding to a nominal 900 MHz CPU clock.

Writable ops require the EN7528 family, 900 MHz boot metadata, and a
recognized idle PLL profile. No EN7528HU-only variant gate is applied.
The 28 profiles have short hardware test evidence on EN7528HU; other
family variants still require hardware validation. Configurations that
fail the boot-rate or initial-profile checks retain reporting only.
CPUFreq discovers only the candidates that the provider rounds exactly;
a read-only provider cannot match two different candidate rates. EN7528
therefore exposes 325 through 1000 MHz in 25 MHz steps. Frequencies through
900 MHz are available without boost; 925, 950, 975 and 1000 MHz require
boost. The initial policy limits are 325 and 900 MHz. The powersave governor
can select 325 MHz, so use userspace for explicit characterization.
EN751221-family devices retain their five-entry table and opt-in 1 GHz
boost; EN7528 pairs are not reused for those devices.

One policy covers all possible CPUs, including all four logical CPUs on
the observed 1004Kc system. The stop-machine callback executes the sequence
once while the other cores and VPEs are stopped: setup ``0x12``, config,
parameter, control ``0x402``, control ``0x406``, then setup zero. Full-state
readback must match; a mismatch attempts to reapply the previous profile.
The EN751221 five-write sequence keeps its existing setup value of ``0x12``.

Use the userspace governor for the first 900 -> 800 -> 900 MHz test::

    policy=/sys/devices/system/cpu/cpufreq/policy0
    cat "$policy/scaling_available_frequencies"
    cat "$policy/related_cpus"
    echo userspace > "$policy/scaling_governor"
    echo 900000 > "$policy/scaling_setspeed"
    echo 800000 > "$policy/scaling_setspeed"
    cat "$policy/cpuinfo_cur_freq"
    echo 900000 > "$policy/scaling_setspeed"

Keep a restoration trap around an actual test workload, and confirm the
configured nominal rates with short pinned PMU windows as on EN7526G.
At nominal 800 MHz the derived BUS rate is 200 MHz; at 700 MHz it is
175 MHz. CCF bus notifications preserve the existing Airoha watchdog
retiming, and MIPS CPUFreq notifiers update delays on every VPE. No DDR
recalibration or voltage adjustment is performed. Automatic dynamic
switching remains disabled. No documented PLL lock or glitchless-switch
guarantee is available; use a serial console and a recoverable test image.

EN7528HU measurement evidence
----------------------------

The completed sweep contains 33 short PMU windows: a 900 MHz reference,
925/950/975 MHz on ascent, all 28 profiles from 1000 down to 325 MHz, and
a restored 900 MHz reference. All workload checksums matched. The largest
relative deviation on descent was approximately 0.00845 percent. One
975 MHz ascent window measured 967.645 MHz, while the same pair measured
975.064 MHz on descent; further measurements are needed to explain that
window difference. The final 900 MHz reference measured 900.044 MHz and
software boost was disabled successfully.

The table is now unconditional; ``CONFIG_ECONET_CPUFREQ_EN7528_RAMP`` is
no longer needed. The CPUFreq driver's union of candidate rates is still
filtered through exact provider rounding. Reporting-only providers retain
one entry and cannot acquire writable ops from the larger candidate list.
No generic PLL arithmetic, voltage control or DDR recalibration is added.
