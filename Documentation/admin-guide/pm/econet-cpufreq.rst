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
700 MHz    0x05101b08   0x02800000   Vendor bootloader
800 MHz    0x05101f08   0x03000000   Interpolated, exercised on EN7526G
900 MHz    0x05102308   0x03800000   Vendor bootloader
1000 MHz   0x05102708   0x04000000   Short PMU windows on EN7526G
========== ============ ============ ================================

The 400 MHz pair was initially extrapolated as 325 MHz, but short PMU
measurements established approximately 400.008 MHz between two 900 MHz
reference measurements. This pair is no longer exposed after a system hang
was reported during automatic switching. The four retained complete pairs
are recognized throughout
the EN751221 family; no other rates are inferred automatically from these
undocumented fields. Measurements on EN7526G do not establish stability
on other family variants.
EN7526C/EN7522 use
a different PLL layout and retain boot-metadata reporting even when they
share the EN751221 clock-controller compatible.

EN7528 uses a different layout: ``0x1fa201a0`` for programming setup,
``0x1fa201ec`` for configuration, ``0x1fa201f8`` for apply control, and
``0x1fa201fc`` for the parameter. An idle recognized profile requires
setup zero and control ``0x406``. The following 17 complete pairs are
recognized without a separate characterization option:

========== ============ ============
CPU        0x1fa201ec   0x1fa201fc
========== ============ ============
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

Profiles below 600 MHz are no longer exposed after a system hang followed
by watchdog recovery was reported with automatic switching. This restriction
does not establish the cause of that hang. Only the retained family rate sets can be programmed through the clock
provider using the characterized encoding.

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
The clock controller exposes 700, 800, 900 and 1000 MHz. These profiles
have short test evidence on EN7526G; enabling them on another family
variant does not establish its operating limits or stability. CPUFreq starts
with a 700 MHz floor and 900 MHz ceiling;
the 1 GHz entry is marked as software boost and disabled by default. This
policy floor is not a claim that 700 MHz is the silicon's minimum frequency.

Enable ``CONFIG_CPU_FREQ_GOV_USERSPACE`` (built in or as a module), then use::

    policy=/sys/devices/system/cpu/cpufreq/policy0
    cat "$policy/scaling_available_frequencies"
    echo userspace > "$policy/scaling_governor"
    echo 700000 > "$policy/scaling_setspeed"
    cat "$policy/cpuinfo_cur_freq"
    echo 900000 > "$policy/scaling_setspeed"

Frequencies are in kHz. The 700 MHz floor applies to supported clock API
requests and CPUFreq selection; root can still bypass both with ``devmem``.
Writable EN751221 and EN7528 policies allow dynamic governors. Reporting-only
policies keep automatic switching disabled. No voltage adjustment or thermal
cooling device is introduced.

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
The 17 retained profiles have short hardware test evidence on EN7528HU; other
family variants still require hardware validation. Configurations that
fail the boot-rate or initial-profile checks retain reporting only.
CPUFreq reads candidates from CPU0's OPP table and keeps only the rates
that the provider rounds exactly;
a read-only provider cannot match two different candidate rates. EN7528
therefore exposes 600 through 1000 MHz in 25 MHz steps. Frequencies through
900 MHz are available without boost; 925, 950, 975 and 1000 MHz require
boost. The initial policy limits are 600 and 900 MHz. EN751221-family devices
retain their four-entry table and opt-in 1 GHz boost. No unvalidated
EN751221 600 MHz entry is added; its lowest retained profile is 700 MHz.

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
recalibration or voltage adjustment is performed. Writable policies allow
automatic dynamic switching. No documented PLL lock or glitchless-switch
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

The retained rate set is unconditional;
``CONFIG_ECONET_CPUFREQ_EN7528_RAMP`` is no longer needed. CPUFreq gets its candidates from the DT OPP table and
filters them through exact provider rounding. Reporting-only providers
retain one entry and cannot acquire writable ops from the OPP table.
No generic PLL arithmetic, voltage control or DDR recalibration is added.

Ondemand governor
-----------------

Enable ``CONFIG_CPU_FREQ_GOV_ONDEMAND=y`` (or load ``cpufreq_ondemand`` when
built as a module). The driver permits dynamic governors only after the
clock provider exposes programmable profiles matching the OPP table. This covers
writable EN751221 and EN7528 policies; reporting-only configurations retain
``CPUFREQ_NO_AUTO_DYNAMIC_SWITCHING`` and fall back to a static governor.

Select ondemand after configuring the shared policy limits::

    policy=/sys/devices/system/cpu/cpufreq/policy0
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    echo 900000 > "$policy/scaling_max_freq"
    cat "$policy/scaling_min_freq"
    cat "$policy/scaling_available_governors"
    echo ondemand > "$policy/scaling_governor"
    cat "$policy/scaling_governor"

The ordinary minimum is 700 MHz on EN751221 and 600 MHz on EN7528. A
600 MHz floor resolves to the next available EN751221 profile, 700 MHz.
Previous userspace limits remain in force within the new supported range.
Rates above 900 MHz remain opt-in software boost frequencies. Ondemand
respects the policy limits and does not enable boost itself.

The driver supplies ``transition_delay_us = 100000`` for writable policies.
This sets an initial 100 ms update interval for ondemand. It is a software
sampling choice to reduce repeated stopped PLL transitions, not a measured
PLL transition or lock time. The reported transition latency remains the
existing generic default. The ondemand ``sampling_rate`` tunable can change
the interval; the driver does not impose a hard 100 ms minimum between
requests. With per-policy governor tunables it is available at::

    cat "$policy/ondemand/sampling_rate"

With global governor tunables use::

    cat /sys/devices/system/cpu/cpufreq/ondemand/sampling_rate

All changes use the existing sleeping ``target_index`` path, stopped PLL
sequence, readback, rollback, BUS/watchdog notifications and MIPS delay
updates. No fast-switch callback is introduced. Clearing the generic flag
also permits other dynamic governors; this change does not select a global
default governor. To select ondemand at boot, configure
``CONFIG_CPU_FREQ_DEFAULT_GOV_ONDEMAND=y``.

The previous short PMU windows establish the selected nominal rates, not
stability under prolonged automatic switching. Check repeated idle/load
transitions and peripheral traffic on each target board.

Restricted operating range after a hang
--------------------------------------

Following a report of a system hang followed by watchdog recovery with
automatic switching, the writable tables exclude every profile below
600 MHz. EN7528 retains 17 profiles from 600 through 1000 MHz. EN751221
retains 700, 800, 900 and 1000 MHz because no 600 MHz profile has yet been
validated on that family. Unsupported low requests are rejected by the
clock provider without programming the PLL. CPUFreq resolves its policy
limits against the retained table, so userspace cannot restore the removed
profiles by lowering ``scaling_min_freq``.

This restriction does not diagnose the hang or establish stability of the
remaining range. Ondemand, its initial 100 ms sampling interval, the 900 MHz
non-boost ceiling and watchdog integration are retained. Reporting-only
configurations, including EN7580, retain their original reported rates even
when those rates are below 600 MHz.

Device Tree operating points
----------------------------

The EN751221 and EN7528 DTSI files provide a standard
``operating-points-v2`` table referenced by both CPU nodes. ``opp-shared``
describes the shared clock domain; the CPUFreq policy still covers all
possible CPUs, including VPEs without a separate CPU node.

The EN751221 table contains 700, 800, 900 and 1000 MHz. The EN7528 table
contains 600 through 1000 MHz in 25 MHz steps. Entries above 900 MHz carry
``turbo-mode`` and remain disabled until software boost is enabled. The
driver also enforces the boost flag above 900 MHz if a board table omits
``turbo-mode``. The tables do not describe voltage scaling and contain no
``opp-microvolt`` properties.

For example, the EN7528 600 MHz entry is::

    cpu_opp_600: opp-600000000 {
        opp-hz = /bits/ 64 <600000000>;
    };

A board can remove or disable individual entries. For example, an
EN751221 board can restrict the policy to 800 and 900 MHz with::

    &cpu_opp_700 {
        status = "disabled";
    };

    &cpu_opp_1000 {
        status = "disabled";
    };

CPUFreq loads the available OPPs on CPU0 at probe. Unsupported rates and
rates that cannot be represented exactly in kHz are disabled before the
OPP helper builds the CPUFreq table. An OPP can restrict the hardware
profiles but cannot introduce new PLL settings or restore a removed low
profile. The PLL field encoding and stopped transition sequence remain in the
clock driver. Runtime OPP table changes are not synchronized with
the already-created CPUFreq table; update the DTB and reboot.

Deploy the updated kernel and DTB together. Without ``operating-points-v2``
on CPU0, this driver reports the current boot rate only. The same fallback
preserves reporting on read-only clocks whose boot rates are absent from
the OPP table. A single matching OPP different from the boot rate can still
be selected through the writable provider. A malformed or empty DT OPP
table is an error, rather than a request for a built-in frequency list.

The generic ``cpufreq-dt`` automatic platform-device path excludes these
two SoC compatibles so it cannot race the EcoNet driver after OPP tables
are added. EcoNet retains its provider checks, boost restrictions, MIPS
delay updates, BUS/watchdog notifications and 100 ms governor update hint.

Boot context and masked PLL programming
--------------------------------------

The register-profile arrays have been removed. For the characterized
25 MHz encoding, the clock provider retains exactly the same rate sets:
EN751221 accepts 700 through 1000 MHz in 100 MHz increments; EN7528 accepts
600 through 1000 MHz in 25 MHz increments. DT OPP tables can restrict these
sets. No additional frequency is enabled by removing the arrays.

For an accepted rate, the configuration count is ``rate / 25000000 - 1``
and the parameter count is seven less. The observed configuration byte is
bits 15:8; the parameter field used by these rates is bits 26:21. A
transition reads the complete current register state inside stop_machine,
replaces only these frequency fields, and retains all other bits. It does
not synthesize a complete ``0x05101708`` register value. The direct divider
encoding in config bits 21:20 must still equal one. Writable registration
also requires the decoded boot fields to agree with the saved 900 MHz
boot rate, rather than accepting inconsistent metadata and register fields.

Live readback validates setup, control, divider, family rate bounds and the
relationship between the two frequency fields. Bits outside the frequency
fields must match the boot snapshot. An unexpected change outside them is
rejected rather than silently interpreted as the same PLL mode. Target
readback compares the full expected state; failure reapplies and verifies
the exact previous snapshot. The five/six-write transition order, watchdog
notifications, MIPS delay handling and software boost checks are retained.

EN7528 XTAL selection
~~~~~~~~~~~~~~~~~~~~

The SDK's EN7528 DDR calibration object chooses the XTAL selector using
bit 9 of the parameters saved at ``0x1fb00284``. When clear, it reads
``0x1fa20174``; when set, it reads ``0x1fa2ff28``. Bit 21 of the selected
register is the selector: one indicates 25 MHz, zero indicates 20 MHz.
The EN7528 SCU node maps the alternate selector as a separate four-byte
``xtal`` resource, adjacent to the xPON TX-off register without overlapping
it. With an older DTB that provides neither this resource nor a CHIP-SCU
range covering the alternate address, that path returns an unknown
reference and frequency control remains disabled.

The supplied short PMU measurements characterize the 25 MHz CPU encoding
only. They do not establish the CPU PLL programming fields or fractional
behavior for a 20 MHz reference. An EN7528 with 20 MHz or unknown XTAL
therefore retains reporting of its saved boot rate without writable PLL
operations. A change from that saved state becomes unknown. The driver
does not scale the 25 MHz formula by a guessed 20/25 factor. EN751221
retains its previously characterized 25 MHz encoding; no new XTAL detector
is inferred for that family.

Preserving other boot bits is not hardware validation of a new PLL mode or
board. Characterizing the 20 MHz CPU path still requires its boot PLL dump
and independent frequency measurements before enabling transitions.
