.. SPDX-License-Identifier: GPL-2.0-only

AB1536U UART bring-up on the Quantum C6500XK
==========================================

Scope
-----

This is an experimental transport test, not a confirmed controller-mode
conversion for the stock firmware. The driver uses UART2 at 115200 baud,
8N1 without RTS/CTS. It consumes RACE frames separately from HCI events;
the old RACE marker 0x05 must not be interpreted as modern HCI ISO data.

It sends the stock firmware version query, an NVKEY read-only probe after
a successful version reply, then a standard HCI Reset.
Successful RACE communication establishes that the UART and application
transport work. Only a successful HCI response allows normal Bluetooth
core initialization to proceed. Even that response does not prove that
all later HCI commands, discovery or connections will work.

No NVKEY is written and no firmware is uploaded. The stock FOTA image
is not a Linux request_firmware() payload for this driver.

Read-only controller-mode probe
-------------------------------

The AB1562 SDK's command_factory.xml defines RACE_NVKEY_READFULLKEY as
command 0x0a00. nvkey_list.h names key 0x3604
NVKEYID_BT_CON_FORCE_CONTROLLER. The test sends this one-byte read::

  05 5a 06 00 00 0a 04 36 01 00

This is a candidate command/key from another chipset's SDK. Neither its
availability nor its meaning is confirmed for the Gemtek AB1536U firmware.
In the reference SDK, a zero key value selects the normal application;
a nonzero value takes the controller initialization path. That path alone
does not establish external HCI routing on this UART.

The receive handler waits for RACE response type 0x5b, ID 0x0a00. It logs
the payload length and up to 32 raw payload bytes at info level. It does
not assume that the first byte is a value or a success status. A received
nonempty response means only that a matching frame arrived; retain the
raw log to establish the layout before interpreting the key. An empty
response reports -EBADMSG. No response within 2.5 seconds reports a timeout,
which does not distinguish unsupported command, missing key or other
transport/application behavior. HCI Reset still runs after this probe.

Keep the existing serdev Bluetooth child enabled and remove the temporary
FOTA GPIO hogs before this test. Retain vendor_high=1, the profile that has
produced valid RACE version replies on this board. No new boot option or
configuration option is needed for the read; it runs once per setup after
a successful version reply. There are no write, erase or FOTA commands.

Expected additional messages include "RACE NVKEY read-only probe", either
"RACE NVKEY 0x3604 reply" or a query failure, and "After NVKEY read".
With the successful version query and no HCI response, normally the tty
accepts 8 bytes after the version query, 18 after the NVKEY query and 22
after HCI Reset. Receive counts depend on the firmware's actual reply.

Kernel configuration
--------------------

Enable these options in the actual OpenWrt/kernel configuration::

  CONFIG_BT=y
  CONFIG_BT_LE=y
  CONFIG_BT_HCIUART=y
  CONFIG_BT_HCIUART_H4=y
  CONFIG_SERIAL_DEV_BUS=y
  CONFIG_SERIAL_DEV_CTRL_TTYPORT=y
  CONFIG_BT_AB1536U=y
  CONFIG_SERIAL_8250=y
  CONFIG_SERIAL_8250_NR_UARTS=4
  CONFIG_SERIAL_8250_RUNTIME_UARTS=4
  CONFIG_SERIAL_OF_PLATFORM=y
  CONFIG_SERIAL_8250_AIROHA=y

BT_AB1536U can also be a module, provided that btab1536u.ko and its
Bluetooth/HCI UART dependencies are included in the root filesystem.
Building into the kernel avoids requiring a new OpenWrt kmod package for
this first test. BT_HCIUART_SERDEV is selected by the existing Kconfig.
GPIOLIB, OF and the EN7580 pinctrl driver must also be enabled.

The board enables the console, UART2 and UART5. A two-port 8250 limit
registers the console and UART2, but UART5 fails with -ENOSPC (-28).
Use at least three ports; four leaves space for another UART. Check that
the boot arguments do not override this with 8250.nr_uarts=2. The UART5
failure does not explain a missing response on an already registered UART2.

Device Tree
-----------

linux(3).tgz contains en7580.dtsi, but not the user's board DTS. The
simplest integration is to append this include to the C6500XK board DTS,
after its existing definitions, and rebuild the DTB and boot FIT::

  #include "en7580-ab1536u-test.dtsi"

This fragment must be available in the DTS include path, alongside
en7580.dtsi. Alternatively, apply the supplied test overlay. The fragment
adds the following child to UART2. Do not add uart-has-rtscts::

  &uart2 {
      status = "okay";

      bluetooth {
          compatible = "gemtek,c6500xk-ab1536u", "airoha,ab1536u";
          max-speed = <115200>;
          gemtek,control-gpios = <&pinctrl 26 GPIO_ACTIVE_HIGH>,
                                 <&pinctrl 28 GPIO_ACTIVE_HIGH>;
      };
  };

The optional control array reproduces the GPIO levels observed in the
stock recovery path: GPIO28 low, GPIO26 high then low. This test uses a
200 ms pulse and a four-second settling delay; the exact pulse duration
is not established by the reverse engineering. The GPIO electrical
roles are not asserted to be RESET/ENABLE/WAKE. Remove the entire
gemtek,control-gpios property to test without changing those levels.
The driver leaves the final levels unchanged when it is removed.

A later stock register snapshot differs from this recovery path:
GPIO DATA=0xfefffbd1 has GPIO26 and GPIO28 high, whereas the first
OpenWrt test had DATA=0xe9fffbd1 with both low. Direction CTRL1 is
0x45150050 in both snapshots, and both pins have output enable set.
Stock IER=0x01 and OpenWrt IER=0x05 both enable UART receive interrupts.
The UART clock divider/XYD pairs calculate approximately 115192 and
115200 baud respectively, assuming a 20 MHz clock and BRDIV=1.

To test those observed high levels with the built-in driver, add this
to the kernel command line in the boot configuration and rebuild the
boot image if that configuration is embedded::

  btab1536u.vendor_high=1

For a module, supply the option when loading the driver::

  modprobe btab1536u vendor_high=1

This opt-in profile drives GPIO28 high, then GPIO26 high, waits four
seconds and issues the same RACE/HCI queries. It does not pulse GPIO26.
The log must report "Control profile: vendor-high" and raw readback
GPIO26=1 GPIO28=1. Collect /proc/cmdline to verify that the boot option
was passed. The parameter is read-only after initialization; changing
it requires loading the module again or another boot. The default
remains the previously tested recovery-low sequence.

This is a hypothesis test, not a confirmed reset or power sequence.
The AB153x family datasheet describes RSTN as active low and REGEN as
a regulator-enable input. Neither signal has been traced to the router
GPIOs. Furthermore, gtk_ble_daemon's "Set BLE Module Off" path ends
with GPIO26 high, while its "Set BLE Module On" path ends with GPIO26
low. Thus a high/high static snapshot may represent an idle or disabled
state; it does not establish that high levels enable this firmware.
No firmware, NVKEY or UART baud configuration changes are made by this
option. To reproduce a known working stock state, record the GPIOs
while a RACE version reply or a BLE connection is actually observed.

The child binds through serdev and takes ownership of UART2. Its tty
node may therefore disappear; do not run gtk_ble_daemon, hciattach or
another serial reader on the same port.

For the overlay route, compile the base DTB with symbols (DTC_FLAGS=-@),
build en7580-ab1536u-test.dtbo, then merge it into the base DTB with
fdtoverlay. Ensure the merged DTB is the one packaged into the boot FIT.
The kernel does not apply this overlay automatically. A standalone build
of the overlay from the kernel source directory is also possible::

  cc -E -P -x assembler-with-cpp -nostdinc -undef -D__DTS__ \
     -I include arch/mips/boot/dts/econet/en7580-ab1536u-test.dtso \
     -o /tmp/en7580-ab1536u-test.dts
  dtc -@ -I dts -O dtb -o /tmp/en7580-ab1536u-test.dtbo \
      /tmp/en7580-ab1536u-test.dts
  fdtoverlay -i base.dtb -o test.dtb /tmp/en7580-ab1536u-test.dtbo

Reading the test result
-----------------------

After boot, collect::

  dmesg | grep -iE 'ab1536|btab1536|bluetooth|race|serial|tty'
  ls /sys/bus/serial/devices
  ls /sys/class/bluetooth
  cat /proc/tty/driver/serial
  cat /proc/interrupts
  cat /proc/cmdline

The diagnostic follow-up logs the first eight receive chunks and the
first four dequeued transmit packets, capped at 32 displayed bytes per
chunk or packet. Counts include traffic since driver probe, including
bytes that the parser cannot recognize. It also logs control GPIO raw
readback after the settling delay. A negative readback is an errno, not
a GPIO level. Readback does not establish the electrical signal at the
Bluetooth module.

The snapshots after RACE and HCI report bytes dequeued by the transport,
bytes accepted by the tty, raw receive bytes, reassembly errors and
packets still queued. They are observations, not synchronized totals.
With no response, normally 8 bytes are accepted after the RACE query and
12 after HCI Reset. A zero receive count establishes that no bytes
reached this receive callback; it does not locate the physical fault.
Bytes accepted by the tty are not proof of transmission on the wire.
The serial driver's /proc counters and an RX/TX capture provide further
evidence. HCI command "tx timeout" means no command response completed;
it does not alone prove that transmission failed.

If BusyBox devmem is available, compare these read-only register reads
between stock and test firmware after their UART initialization::

  devmem 0x1fbf0324 32
  devmem 0x1fbf0328 32
  devmem 0x1fbf032c 32
  devmem 0x1fa20214 32
  devmem 0x1fa20218 32

The first three are UART2 MISCC, XINCLK divider and XYD. The SDK writes
MISCC=0 for no hardware flow control; the current Airoha 8250 baud helper
does not configure that register. This is a comparison point, not an
established cause. For the current driver's 115200 calculation, the
divider is 4 and XYD is 0xea00fde8. Stock may use another valid divider
pair. The last two are pinmux registers: bit 13 at 0x1fa20214 selects
PCM2, and bits 3/4/11 at 0x1fa20218 select UART2, UART2 CTS/RTS and
SIPO RCLK. These modes share some pins; preserve the readouts for
comparison instead of writing registers behind the active drivers.

The initial transport message confirms driver binding and UART setup.
"AB1536U RACE firmware 0.20.0.0" confirms the stock version response.
"AB1536U standard HCI Reset succeeded" confirms a standard command
response. The Bluetooth core will subsequently issue its own reset and
capability queries.

If RACE succeeds but HCI Reset fails, preserve that log: it distinguishes
a working application transport from an unconfirmed external HCI mode.
This patch does not synthesize controller capabilities or an HCI device
from the proprietary BLE application API.

If both requests time out, examine UART2 RX/TX, IRQ activity, the XYD
baud divider, pinmux and GPIO levels. If no driver binding message
appears, check the final boot DTB and the built-in/module configuration
first. The existing serial driver is used without register changes.

For detailed RACE ID/type messages, enable dynamic debug when available::

  mount -t debugfs none /sys/kernel/debug
  echo 'file btab1536u.c +p' > /sys/kernel/debug/dynamic_debug/control

With working HCI initialization and installed BlueZ tools, collect::

  btmgmt info
  btmgmt power on
  btmgmt find -l

Driver registration alone, or the presence of hci0, does not establish
that the stock firmware is usable by BlueZ. Preserve the complete boot
Bluetooth log if a later HCI command fails.
