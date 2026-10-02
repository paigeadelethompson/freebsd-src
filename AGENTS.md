# AGENTS.md

## Project

Native FreeBSD support for the Microsoft Surface Laptop 4. Work happens on the
local branch `surfacelaptop4`. Kernel work goes under `sys/`; all new driver
code lives in `sys/dev/surface/` (currently the SAM/SSH transport, wired as
optional device `surface_sam`).

## Ground rules

- **Native FreeBSD code only.** No Linux driver compatibility layer, no
  linuxulator, no copying of GPL code. Linux sources are *reference material*
  for hardware behavior (register maps, protocol framing, ACPI/DSM quirks).
- Reference tree: `/mnt/linux-surface`. Patches grouped by kernel version
  (`patches/6.6/` … `patches/6.19/`); most relevant: `0007-surface-sam`,
  `0008-surface-sam-over-hid`, `0009-surface-button`, `0012-surface-gpe`,
  `0016-hid-surface`, `0005-ipts`, `0006-ithc`, `0014-amd-gpio`, `0015-rtc`.
  Read for hardware facts only — reimplement in FreeBSD style; do not
  translate files, comments, or function names.
- Drivers should stay **out-of-tree friendly**: keep all code in
  `sys/dev/surface/`, make core-tree edits minimal (one `sys/conf/files`
  block + `sys/modules/` wiring at most), and provide a standalone kmod
  Makefile so the directory can be lifted out.
- Do not commit unless explicitly asked. No drive-by refactors.
- **Do not run kernel builds on this host.** The user runs `buildkernel`
  themselves; agents verify new files with `checkstyle9.pl -f` only and
  report that compile verification is pending.

## Verified commands (this host is Linux, Debian trixie, root)

    perl tools/build/checkstyle9.pl HEAD~1..HEAD          # style, works
    perl tools/build/checkstyle9.pl -f <file>             # single file
    perl tools/build/checkstyle9.pl --github <base>..<head>

Cross-build the kernel — **user-run, agents must not execute this** (kept
here for the user's reference; clang/lld/bmake/libarchive-dev are installed
via apt):

    export MAKEOBJDIRPREFIX=${PWD%/*}/build                # outside the tree
    ./tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 -n
    ./tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 kernel-toolchain -j$(nproc) -DWITH_DISK_IMAGE_TOOLS_BOOTSTRAP
    ./tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 KERNCONF=GENERIC NO_MODULES=yes buildkernel -j$(nproc)

After a first `buildkernel`, single-file rebuild:
`make -C $MAKEOBJDIRPREFIX/.../sys/GENERIC <basename>.o`.

Tests are Kyua/ATF under `tests/` — need a running FreeBSD, not runnable here.
Man pages: `mandoc -Tlint` (mandoc not installed). Report exactly what was and
was not verified.

## Current state: SAM transport + HID/monitor/RTC clients

Files (all pass `checkstyle9.pl -f`, none compiled, no hardware testing;
transport committed as `5b7b1532d182`, HID work committed as `7dc02379eaa5`,
the three new clients and the FN-key filter uncommitted):

- `sys/dev/surface/surface_sam.h` — transport API: `surface_sam_get()`,
  `surface_sam_request(sam, tc, tid, iid, cid, wdata, wlen, rdata, rlen,
  timeout_ms)`, `surface_sam_register_event(sam, tc, fn, arg)` /
  `surface_sam_unregister_event()`. Event handlers run on a private
  taskqueue thread (sam_mtx released); they may issue requests but must not
  do so from the rx path. `MALLOC_DECLARE(M_SURFACE_SAM)`. Plus the HID node
  table API: `struct surface_sam_hid_node { uint8_t hid_tid, hid_iid; }`,
  `surface_sam_hid_node(unit, node)` → KIP tid 0x02, iid 0x01/0x03/0x05
  (keyboard/touchpad/aux), ENOENT past end.
- `sys/dev/surface/surface_sam_uart.{h,c}` — minimal 16550 backend: takes a
  parsed `struct sam_uart_config` (iobase/nports/irq/trigger/polarity/baud/
  rclk/lcr/hwflow), synchronous `sam_uart_tx()` (busy-waits THRE via
  `DELAY(10)`, 2 s cap → EIO), RX via IRQ thread (`INTR_TYPE_TTY |
  INTR_MPSAFE`, no filter) with a 1-tick callout poll fallback when no IRQ
  is available. Divisor math defaults: rclk 1843200, baud 115200.
- `sys/dev/surface/surface_sam.c` — ACPI probe/attach on `MSHW0084`
  (probe returns `BUS_PROBE_DEFAULT + 1` to out-rank `uart(4)`: in
  `device_probe_child` the probe result *closest to 0* wins, so -19 beats
  -20). `_CRS` walked with `AcpiWalkResources()` (UART SerialBus params,
  IO/FixedIO, IRQ/ExtendedIrq, GenericRegister in SystemIo space), I/O
  resource allocated with `bus_set_resource()` fallback if not already in
  the child's resource list. One `MTX_DEF|MTX_SLEEP` mutex serialises TX,
  RX and request state; ACK wait 1000 ms × 3 tries, 8-entry duplicate
  sequence window, one outstanding request, u8 tx seq (chosen once per
  message, retries reuse it), rqid counter 0 then 39+. Attach sequence:
  firmware version (tc SAM, cid 0x13, le32, **fatal**) → D0-entry (cid
  0x34, warn-only) → display-on (cid 0x16, warn-only). Sysctls:
  `firmware_version` and `stats.*` (tx/rx/dup/crc/bad/unexpected/events/
  ack_timeouts/naks/timeouts/tx_errors). Post-commit deltas (uncommitted):
  - responses matched against `sam_rqst_rqid` (stale/mismatched dropped);
  - flagless requests (`rdata == NULL`) complete on ACK — required for
    D0-entry/display-on/output/event-enable, matches Linux
    `SSAM_REQUEST_HAS_RESPONSE` semantics (flag = wait for response frame);
  - attach creates one `surface_hid` child per HID node (unit = iid) and
    calls `bus_attach_children()`; detach calls `bus_generic_detach()`
    first so children die before the transport.
- `sys/dev/surface/surface_hid.c` — HID-over-SAM transport (tc 0x15,
  tid = KIP 0x02): probe is a unit→iid table lookup; attach fetches the
  report descriptor (CID 0x04 get_descriptor, entry 1) with the slice
  protocol (10-byte header `{u8 entry; le32 offset; le32 length; u8 end;}`,
  max chunk 0x76; entry 0 = 9-byte HID desc validated: desc_len==9, type
  0x21, num==1, report type 0x22; entry 2 = 32-byte attributes →
  `hid_device_info` with `BUS_HOST`, vendor/product/version, rdescsize,
  name "Microsoft Surface %04X:%04X"), then registers the tc event handler
  and attaches a `hidbus` child (`device_set_ivars(child, &sc->sh_hw)`).
  Events: one tc-level handler demuxes by iid through `surface_hid_by_iid[]`
  (guarded by a demux mutex + `surface_hid_handlers` refcount); events are
  enabled/disabled in `hid_intr_setup`/`hid_intr_stop` via REG tc 0x21 cid
  0x01/0x02 (5-byte `{tc=0x15, flags=0, le16 rqid=0x15, iid}` payload,
  nonzero status → EIO), so event traffic exists only while a client is
  open. cid 0 on tc 0x15 = input report → hidbus intr handler. hid methods
  (iichid-shaped): `get_rdesc` from cache; write → cid 0x01 verbatim;
  `get_report` FEATURE → cid 0x02 (payload = report-id byte, response =
  report); `set_report` OUTPUT → cid 0x01 / FEATURE → cid 0x03 (report id
  already in byte 0, sent verbatim); read/idle/protocol/poll/ioctl →
  ENOTSUP/ENOTTY. Zero-progress slice stall → EPROTO.
- `sys/dev/surface/surface_profile.c` — performance profile (tc TMP 0x03,
  tid SAM 0x01, iid 0x00): GET cid 0x02 / SET cid 0x03 (le32, response
  `struct ssam_tmp_profile_info` = `{le32 profile; le16 unk1; le16 unk2}`,
  8 bytes), plus the fan profile on tc FAN 0x05, tid 0x01, iid 0x01, cid 0x0e
  as a single u8 **with a different scale** (FAN: 1 = battery saver,
  2 = normal, 3/4 = better/best — TMP has 1/2 the other way round). Fan
  write failure is logged only (SL4 13" may be fanless). Profiles 1..4 are
  exposed raw through `dev.surface_sam.<unit>.profile`; the kernel 2-state
  `power_profile` is bridged both ways: ECONOMY ⇄ 2, PERFORMANCE ⇄ 4 (an EC
  value of 1 or 3 reads back as PERFORMANCE). Attach syncs the global state
  from the EC; external changes (acpi_acad on AC events) arrive via
  `EVENTHANDLER_REGISTER(power_profile_change, …)` and are applied on a
  private taskqueue thread because SAM requests sleep.
- `sys/dev/surface/surface_mon.c` — sensors, read live on every sysctl
  access (the EC pushes nothing). Sensors: GET available bitmap cid 0x04 on
  TMP (tid 0x01, iid 0x00) → bit n = channel n; channel n lives on iid n+1;
  temperature cid 0x01 (le16, tenths of Kelvin), name cid 0x0e (21 bytes =
  `{le16 unk; u8 unk; char name[18]}`). Fan: RPM cid 0x01 on tc FAN 0x05,
  tid 0x01, iid 0x01; a machine without a fan simply does not answer at
  attach time and gets no `fan.rpm` node. Sysctls under the device tree:
  `temp<n>.{name,temperature}` (temperature in 1/10 K, same convention as
  `hw.acpi.thermal.tz*`) and `fan.rpm`. This tree has **no sysmon_envsys**,
  so sensors are sysctls, as in `acpi_thermal`.
- `sys/dev/surface/surface_rtc.c` — SAM battery-backed clock: GET cid 0x10 /
  SET cid 0x0f on tc SAM 0x01, tid 0x01, iid 0x00, le32 Unix time (this
  supersedes the older register-based 0x00–0x04 / `wHour…wYear` description
  found in early linux-surface patches). Registers as a clock(9) device via
  `clock_register(dev, 1000000)` plus `clock_gettime`/`clock_settime`, and
  also exposes `dev.surface_sam.<unit>.epoch` (CTLTYPE_LONG). Note: it
  attaches long after `inittodr()` has run (ACPI children probe during
  `probe_all`), so it does **not** provide the boot time — its value is
  writing time back to the EC so a later boot (or another OS) starts from the
  right clock. `resettodr()` writes to it, so `date` and `tzsetup` reach the
  EC.
- Wiring: `sys/conf/files` — `dev/surface/surface_hid.c	optional
  surface_sam		hid`, `dev/surface/surface_{mon,profile,rtc}.c	optional
  surface_sam` (no extra idents needed; `kern/clock_if.m` is standard);
  `sys/modules/surface_sam/Makefile` adds those three files and `clock_if.h`
  (generated interface headers must be in SRCS); `SUBDIR+= surface_sam` in
  `sys/modules/Makefile`; `device surface_sam` in `sys/x86/conf/NOTES` (NOT
  GENERIC — same precedent as the acpi_* extras); `device hconf` + `device
  hmt` added to `sys/amd64/conf/GENERIC` (hmt.c is `optional hmt hconf` = AND,
  and hmt.c calls hconf_set_input_mode — touchpad needs both). All three new
  drivers are `surface_sam` children created in the transport's attach
  (unit 0); a child whose function the firmware lacks just fails to attach.
- FN-key filter (`0016-hid-surface*.patch`) is now **implemented** in
  `surface_hid.c`: the firmware reports the FN key as button-page usage
  0x100 (evdev `BTN_0`, EV_KEY 0x100 — not the button-page "Button 1" that
  `hmt` would map to BTN_LEFT), which is how it gets stuck pressed. After the
  report descriptor is fetched, `hidbus_locate()` is used to find that usage
  in every top-level collection (≤ 4), and `surface_hid_deliver()` masks the
  bits in a per-node copy of the report before handing it to hidbus.
  `hid_location.pos` is a bit offset *behind* the report ID byte, so the
  offset is shifted when `hid_report_size_max()` reports numbered reports.
  The touchpad (iid 3) is skipped — its buttons are real input. AUX node
  (iid 5) is still created but has no known consumer.

Known risks to check first on real hardware: whether `MSHW0084` has a
PNP0501 CID (then `uart(4)` probes first and its probe-time I/O
allocation is *not* released when it loses the auction → our attach would
fail EBUSY), the actual `_CRS` contents/IRQ, and the UART reference clock
(override with `hw.surface_sam.rclk` if the divisor computes to 0).
Also unverified: EC status for flagless requests (late/mismatched response
frames are dropped by the rqid guard, but a non-zero ACK-era status byte is
not available for them). Unverified for the new clients: whether the EC
answers the TMP profile get/set on SL4 at all (profile values 1..4 assumed
from upstream), whether the fan answers on the 13" model, and whether the
FN-key usage is really button 0x100 in the descriptor (the filter simply
finds nothing and does nothing if the firmware uses a different usage).



## Surface Laptop 4 hardware facts (gathered 2026-09-30)

Sources: `/mnt/linux-surface/patches/*`, upstream Linux
(`torvalds/linux` master) read via raw.githubusercontent for facts only.

- **SAM/SSAM is present and speaks SSH-over-UART** (not SAM-over-HID; that is
  only gen-4/Pro 4/Book 1). SL4 is in the platform-hub match table:
  - `MSHW0250` = SL4 13" Intel, `MSHW0110` = SL3 15" AMD **and SL4 15" AMD**
    → node group `ssam_node_group_sl3` (battery AC + main, tmp perf profile,
    HID main keyboard/touchpad/iid5).
  - SAM **serial hub ACPI HID = `MSHW0084`** (the serdev/UART device itself;
    this is what a FreeBSD transport driver must attach to).
- **SSH protocol facts** (from upstream `include/linux/surface_aggregator/
  serial_hub.h`, CRC via `crc_itu_t(0xffff, …)` = CRC-16/CCITT-FALSE):
  - message = `SYN(0x55AA, 2B)` + frame(4B) + frameCRC(2B) + payload + payloadCRC(2B).
  - `struct ssh_frame { u8 type; le16 len; u8 seq; }`; types:
    `DATA_SEQ 0x80`, `DATA_NSQ 0x00`, `ACK 0x40`, `NAK 0x04`.
  - command payload: `struct ssh_command { u8 type=0x80, tc, tid, sid, iid;
    le16 rqid; u8 cid; }` (8B). TIDs: HOST 0x00, SAM 0x01, KIP 0x02.
    ACK/NAK messages are `SYN + frame(type, len=0, seq) + frameCRC +
    payloadCRC(empty)=0xffff LE` — the trailing payload CRC is present even
    with no payload. Frame CRC covers the 4 frame bytes; payload CRC covers
    cmd header + data. NAK always uses seq 0. On CRC error the reference
    host skips to after SYN and sends NAK. Transmission: ACK timeout
    1000 ms, 3 attempts, one outstanding packet; retransmissions reuse the
    same seq, each new message gets the next u8 seq (first seq = 0).
  - rqid: counter starts at **0** for the first request, then 39, 40, …
    (values 1..38 are reserved: `event = rqid - 1`). Events are enabled per
    target category with `rqid = tc` (`ssh_tc_to_rqid(tc) == tc`), so event
    frames arrive keyed by tc — the transport dispatches on that.
  - target categories (`enum ssam_ssh_tc`): SAM 0x01 (RTC), BAT 0x02,
    TMP 0x03, FAN 0x05, KBD 0x08, KIP 0x0e, SEN 0x12, HID 0x15, BKL 0x17,
    POS 0x26 … (full list in that header).
  - node names `ssam:<reg>:<category>:<target>:<instance>:<function>` hex,
    e.g. `ssam:01:15:02:01:00` = HID keyboard on KIP hub;
    `ssam:01:03:01:00:01` = temp perf profile.
  - UART params come from ACPI `_CRS` SerialBus resource (baud/parity/flow);
    Linux fallback if no ACPI: 4 MHz baud, HW flow, no parity. Startup
    sequence: open → setup → get firmware version → D0-entry notify →
    display-on notify → GPIO wake IRQ.
- **No touchscreen on SL4** (Laptop line is non-touch) → `ipts`/`ithc`
  (TGL-LP `0xa0d0/0xa0d1`) are *not* a SL4 target; skip those 170 KB patches.
- **AMD SL4 quirk** (`0014-amd-gpio.patch`): firmware MADT is missing the
  legacy IRQ7 override for the power/GPIO button; Linux injects
  `mp_override_legacy_irq(7, 3, 3, 7)`. DMI SKUs:
  `Surface_Laptop_4_1952:1953` (15"), `Surface_Laptop_4_1958:1959` (13").
  FreeBSD equivalent lives in x86 MADT/ioapic setup — core-tree, not
  out-of-tree-able.
- **Keyboard/touchpad** enumerate as HID with `BUS_HOST`, vendor 0x045e,
  products `0x09AE` (keyboard) / `0x09AF` (touchpad) — i.e. behind SAM
  (`surface-hid`), not plain USB. Linux fix `hid-surface.c` filters a stuck
  `BTN_0` (FN key) from SAM HID firmware. Also `0x0C46` = Surface typecover
  touchpad (hid-multitouch quirk), `0x09b5`/`0x09c0`/`0x0925` = typecover.
- **Buttons**: `MSHW0040` power/volume (DSM UUID rev 0x01, function 0x02 =
  OPR check; Linux patch drops the OPR requirement), `MSHW0028` older.
- **Lid GPE** driver (`surface_gpe`) knows GPEs 0x4B/0x4F/0x52/0x57 for
  Pro/Book — **no SL4 entry known**; SL4 lid GPE still unknown (needs the
  machine's DSDT / `acpidump`).
- **WiFi**: mwifiex quirk table only covers Laptop 1/2; ath10k = SL3 AMD.
  SL4 wireless unconfirmed from patches (likely Intel AX201 CNVio2 on Intel
  SKUs → not in these patches at all).
- **RTC**: two variants — ACPI `acpi_tad` (needs the "HW-reduced platforms"
  allowance, `0015-rtc.patch`) and SAM RTC (`SSAM_SDEV(SAM, SAM, 0, 0)`,
  regs 0x00 time / 0x01 alarm / 0x02 enable / 0x03 irq status / 0x04
  seconds-per-tick, 6-byte `wHour…wYear` response, writes padded to 32 B).
- **Gaps we still do not know** (need `acpidump`/`dmesg` from the actual
  machine): SL4 lid GPE, exact `_CRS` of `MSHW0084`, whether Intel SL4 uses
  I2C-HID touchpad or SAM HID only, WiFi chipset, audio codec quirks.

## FreeBSD tree facts (how to plug drivers in)

- **ACPI driver pattern** (see `sys/dev/acpi_support/acpi_ibm.c`,
  `acpi_panasonic.c`, `sys/dev/ichiic/ig4_acpi.c`): static `char *ids[]`,
  `ACPI_ID_PROBE(device_get_parent(dev), dev, ids, NULL)` in `probe`,
  `DRIVER_MODULE(foo, acpi, …)` + `MODULE_DEPEND(foo, acpi, 1, 1, 1)` +
  `ACPI_PNP_INFO(ids)`. Notify: `AcpiInstallNotifyHandler(handle,
  ACPI_DEVICE_NOTIFY, cb, sc)` + `AcpiOsExecute(OSL_NOTIFY_HANDLER, …)`.
- **Sources wiring**: `sys/conf/files` line
  `dev/surface/<file>.c <tab> optional <cfgname> acpi`.
  Modules: `sys/modules/<name>/Makefile` (`KMOD`, `SRCS= … device_if.h
  bus_if.h acpi_if.h opt_acpi.h`), `SUBDIR` entry in `sys/modules/Makefile`.
  Optional devices get `device <name>` in `sys/x86/conf/NOTES`; GENERIC
  changes only if it should be in GENERIC.
- **evdev**: `sys/dev/evdev/` — `evdev_alloc()`, `evdev_set_id(BUS_HOST,…)`,
  `evdev_support_event(EV_SYN/EV_KEY)`, `evdev_support_key(KEY_*)`,
  `evdev_register()`, `evdev_push_key()/evdev_sync()` (`sys/dev/evdev/evdev.h`).
  Guard with `#ifdef EVDEV_SUPPORT`.
- **HID**: transport-agnostic bus `sys/dev/hid/hidbus.{c,h}`; class drivers
  match with `HID_TLC(page, usage)` + `HIDBUS_LOOKUP_DRIVER_INFO`, register
  `DRIVER_MODULE(name, hidbus, …)`; input mapping via `hidmap.c`. USB transport
  `sys/dev/usb/input/usbhid.c`; **I2C-HID** `sys/dev/iicbus/iichid.c`
  (`optional iichid acpi hid iicbus`, attaches a `hidbus` child). Generic
  input chain: hidbus → hmt/hpen/hkbd → hidmap → evdev → `/dev/input/eventN`.
- **No serdev bus.** `sys/kern/serdev_if.m` is only puc/scc interrupt glue;
  nothing hands a UART to a sibling driver. A SAM transport must either
  attach to the ACPI serial device itself and drive the 8250 registers via
  `bus_space`, or lose the device to `uart(4)` (`uart_bus_acpi.c` matches
  PNP0501-class HIDs, so `MSHW0084` should be free for us).
- **UART**: `sys/dev/uart/` (`uart_bus_acpi.c`, `uart_dev_ns8250.c`,
  `uart_if.m`); `device uart` + `device uart_ns8250` are already in
  GENERIC/DEFAULTS. There is **no kernel tty-client API** — do not plan on
  talking to `/dev/ttyu*` from a driver.
- **No platform-profile mechanism** exists (Linux `platform_profile` has no
  FreeBSD analogue). What exists: `power_profile_get/set_state()` +
  `EVENTHANDLER_REGISTER(power_profile_change, …)` (`sys/sys/power.h`,
  `sys/kern/subr_power.c`); only `acpi_acad.c` currently sets it. A SAM
  performance-profile driver would add a sysctl writer + that eventhandler.
- **EC/battery/thermal already exist** (`acpi_ec.c`, `acpi_battery.c`,
  `acpi_acad.c`, `acpi_thermal.c`, `acpi_lid.c`, `acpi_button.c` — all
  `optional acpi`, built in). Don't rewrite these; SAM clients only add what
  ACPI can't provide (perf profile, fans/sensors; HID-over-SAM is done).
- **sysctl/lock conventions**: `SYSCTL_ADD_*` under
  `device_get_sysctl_ctx/tree(dev)`; ACPI drivers use a subsystem mutex +
  `ACPI_SERIAL_BEGIN/END`.

## Process

1. Implement the smallest working piece first (SAM transport → sync request
   API → client drivers one at a time).
2. Verify with `checkstyle9.pl -f` on every new file; compilation is the
   user's `buildkernel` run — report exactly what was and was not
   verified, never claim something works without it.

Next candidates: hardware bring-up items (see the gaps list above) — every
driver is code-complete but unbuilt and untested — first: user `buildkernel`,
then `kldload`/probe on real hardware (check dmesg for `surface_sam`/
`surface_hid`/`surface_profile`/`surface_mon`/`surface_rtc`, hkbd/hmt attach,
`/dev/input/event*`, `sysctl dev.surface_sam.0.*`). Hardware bring-up items
need `acpidump`/`dmesg` from the actual machine (see gaps list above).
