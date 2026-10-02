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
  rclk/base_freq/lcr/hwflow), synchronous `sam_uart_tx()` (busy-waits THRE,
  bounded by an iteration count rather than by `ticks`, because the clock
  does not run during cold boot), RX via IRQ thread (`INTR_TYPE_TTY |
  INTR_MPSAFE`, no filter). **No polling fallback**: at 4 MBd a 1-tick poll
  cannot drain the receive FIFO, so without an interrupt there is no
  attach. `sam_uart_lpss_init()` brings an Intel LPSS UART up (below),
  `sam_uart_lpss_set_clock()` scales it to 16x the requested baud, and the
  divisor falls back to the integer divisor when the core has no fractional
  latch, warning loudly when the resulting rate is wrong.
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

Still unverified: EC status for flagless requests (late/mismatched response
frames are dropped by the rqid guard, but a non-zero ACK-era status byte is
not available for them). Unverified for the new clients: whether the EC
answers the TMP profile get/set on SL4 at all (profile values 1..4 assumed
from upstream), whether the fan answers on the 13" model, and whether the
FN-key usage is really button 0x100 in the descriptor (the filter simply
finds nothing and does nothing if the firmware uses a different usage).
Also: the transport has no suspend/resume, while the firmware says it
closes the UART in D3 (`SSH._DSM` fn 8, and UA00's `_PS0` is empty), so
after S3 the link is dead until the module is reloaded.


## Bring-up log: the LPSS UART comes alive (2026-10-02)

Transport bring-up committed as `79d3eeaa6c92`.  First boot where the whole
window resolves, the controller comes out of reset, the baud rate comes out
exactly right and the interrupt is actually wired up:

```
surface_sam0: <Microsoft Surface Serial Hub (SAM)> on acpi0
surface_sam0: _CRS: uart baud 4000000 hwflow 1 lcr 0x3, controller \_SB.PCI0.UA00
surface_sam0: _CRS: resource type 17
surface_sam0: _CRS: resource type 17
surface_sam0: _CRS: resource type 7
surface_sam0: following resource source \_SB.PCI0.UA00
surface_sam0: \_SB.PCI0.UA00: no window in _CRS, trying its pci function
surface_sam0: \_SB.PCI0.UA00: ADR 0x1e0000 -> bus 0 slot 0x1e func 0
surface_sam0: \_SB.PCI0.UA00: BAR 0 has no base; the bus will assign it
surface_sam0: \_SB.PCI0.UA00: intpin 1, intline 255
surface_sam0: \_SB.PCI0.UA00: using BAR 0 at 00:1e.0, reference clock 120000000 Hz
surface_sam0: _CRS asks for hw flow control, leaving it off (hw.surface_sam.hwflow=1 to enable)
surface_sam0: resolved window: memory BAR 0 on the uart function
surface_sam0: resolved irq: from the pci bus (trigger and polarity come from its routing)
surface_sam0: resolved line: baud 4000000 lcr 0x3 hwflow 0
pcib0: allocated type 3 (0x95500000-0x95500fff) for rid 10 of pci0:0:30:0
unknown: Lazy allocation of 0x1000 bytes rid 0x10 type 3 at 0x95500000
surface_sam0: window memory 0x95500000-0x95500fff (4096 bytes)
surface_sam0: lpss clock register 0, base 120000000 Hz
surface_sam0: lpss uart out of reset (caps 0x10, resets 0x7, clock 120000000 Hz)
pcib0: matched entry for 0.30.INTA
pcib0: slot 30 INTA hardwired to IRQ 20
surface_sam0: interrupt 20, routed by the pci bus
surface_sam0: fractional divisor latch 0xc0: read 0 with all ones written
surface_sam0: clock set to 64000000 Hz (ratio 8/15)
surface_sam0: uart @ 0x95500000, 32-bit regs (shift 2), baud 4000000 (div 1 of rclk 64000000 = 4000000), no hwflow
ioapic0: routing intpin 20 (PCI IRQ 20) to lapic 6 vector 51
surface_sam0: failed to get SAM firmware version (error 60)
device_attach: surface_sam0 attach returned 6
acpi_lid0: <Control Method Lid Switch> on acpi0
```

What each of those lines establishes, in the order the problems were found:

1. `ADR 0x1e0000` → 00:1e.0.  The firmware encodes the PCI function in the
   *low* byte of `_ADR` (UA01 = 0x001E0001, UA02 = 0x00190002), which breaks
   the spec's "bits 15:8 = function, bits 7:0 = 0".  Decode the spec field
   first and fall back to the low byte.
2. `BAR 0 has no base` → the bus assigns it.  The firmware never programs
   the LPSS BARs; `pci_alloc_resource()` falls through to
   `pci_reserve_map()`, which sizes the BAR, reserves a range and writes it.
   A zero BAR is not an error, it just has to be requested.
3. UA00 has **no `_CRS` at all** (only `_DSM`, `_ADR`, `_PS3`, `_PS0`), so the
   window can only come from the PCI BAR.  `_PS3` is `SOD3(UC00, One, One)`
   and `_PS0` is empty: the firmware puts the block in a low power state and
   never restores it, so the driver has to program it itself.
4. `reference clock 120000000 Hz` → the PCI device id matters.  This function
   is `0x34a8` ("Ice Lake-LP Serial IO UART Controller"), which the reference
   driver puts in the *Sunrisepoint* group at **120 MHz**; 100 MHz is the Bay
   Trail/Tiger Lake group.  4 MBd off 120 MHz is 1.875, not a divisor.
5. `lpss uart out of reset (caps 0x10, resets 0x7)` → the additional
   register block at 0x200 in the BAR is real and writable.  It is programmed
   exactly as the PCH documentation of that block describes it: reset pulse,
   then release functional and iDMA (0x204 = FUNC|IDMA), then a **single 32
   bit** write of the window address to 0x240.  (An 64 bit write there spills
   into 0x244 and knocks the block back into reset — that is what produced
   an `internal timer error` machine check.)
6. `fractional divisor latch 0xc0: read 0` → this core has no fractional
   divisor latch, so the clock itself has to be scaled: `clock set to
   64000000 Hz (ratio 8/15)` then `div 1 of rclk 64000000 = 4000000`.  The
   ratio write is verified by reading the register back and the previous
   value is restored if it does not take.
7. `interrupt 20, routed by the pci bus` + `ioapic0: routing intpin 20` →
   the interrupt works, once it is claimed on *our* device.  A PCI function
   with no driver never joins a devclass, so it has no `nameunit`
   (`subr_bus.c:1314`), and `nexus_setup_intr()` passes that NULL name
   straight to `intr_add_handler()`, which refuses it with EINVAL.  Two
   earlier diagnoses of this were wrong: it is not "MSI only" (every driver on
   this box is MSI, but MSI goes through the same nameless function), and
   `struct intsrc.is_event` *is* created at boot by `intr_register_source()`.

Where it stands: window, controller, baud rate and interrupt are all correct
now, and the failure has moved to the protocol — the EC does not answer the
firmware-version request, `error 60` is `ETIMEDOUT`.  Nothing in the log says
whether bytes left the port, so the next step is diagnostics, not another
guess: expose the statistics sysctls *before* the first request (they are
added after it now, so a failed attach leaves nothing to read), say which
wait timed out (ACK or response), print the line status after a transmit
(0xff would mean the functional block is still not running), and dump the
statistics on attach failure.



## Surface Laptop 4 hardware facts (gathered 2026-09-30)

Sources: `/mnt/linux-surface/patches/*`, upstream Linux
(`torvalds/linux` master) read via raw.githubusercontent for facts only.

- **The SSH UART is an Intel LPSS (DesignWare) core on PCI**:
  `pci0:0:30:0 = 8086:34a8` "Ice Lake-LP Serial IO UART Controller", which the
  reference driver groups with Sunrisepoint at **120 MHz** (100 MHz is the Bay
  Trail/Tiger Lake group).  UA00 (`00:1e.0`) has no `_CRS`, so its window is
  BAR0 only, and its additional register block sits at **0x200** in that BAR
  (reset control 0x204 = FUNC|IDMA, register address 0x240 as a single 32 bit
  write, clock ratio at 0x200 with M in 15:1 and N in 31:16, gate in bit 0,
  device type in 7:4 of 0x2fc).  MSHW0084 is `\_SB.SSH`, whose `_CRS` is a
  prebuilt buffer (`SBUF`) whose baud field `_INI` fills from `\PSBR`, the PCH
  UART's own baud register at offset 0x7ed of the PCH power management region —
  4,000,000 on this machine, and 4 MBd needs a clock of exactly 16x that.
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
- **This tree's `bus_setup_intr()` flag values are a trap** (`sys/sys/bus.h:269`):
  `INTR_TYPE_TTY = 1`, `INTR_TYPE_BIO = 2`, `INTR_TYPE_NET = 4`, …,
  `INTR_MPSAFE = 512`, while trigger and polarity are separate enums with the
  *same* values (`INTR_TRIGGER_EDGE = 1`, `INTR_TRIGGER_LEVEL = 2`,
  `INTR_POLARITY_LOW = 2`).  `intr_priority()` masks with the type bits only and
  panics ("no interrupt type in flags") unless exactly one type bit is set, so
  passing `INTR_TRIGGER_LEVEL` alongside `INTR_TYPE_TTY` yields 5 and panics.
  Trigger and polarity are configured per source by the interrupt *routing*
  (via `BUS_CONFIG_INTR`), not per handler.
- **A device that never joined a devclass has no `nameunit`** — it is only
  allocated in `devclass_add_device()` (`subr_bus.c:1314`).  A PCI function with
  no driver of its own therefore cannot own an interrupt handler:
  `nexus_setup_intr()` passes `device_get_nameunit(child)` to
  `intr_add_handler()`, and `intr_event_add_handler()` refuses a NULL name with
  EINVAL.  Claim the interrupt on the device that *does* have a name.
  (`struct intsrc.is_event` is not the problem: `intr_register_source()` creates
  it for every source at boot.)
- **No timed sleep on thread0 during cold boot**: ACPI devices attach from
  `root_bus_configure()`, where `sleepq_set_timeout_sbt()` panics with "timed
  sleep before timers are working" (`subr_sleepqueue.c:408`, `if (cold && td ==
  &thread0)`).  Anything the driver does in attach must spin rather than
  `msleep()`, and must not measure timeouts in `ticks`, which does not advance
  yet either.  Test with `if (!cold)`.
- **`pci_get_bus/slot/function/domain/intpin/…` are static inlines** generated
  by `PCI_ACCESSOR()` in `sys/dev/pci/pcivar.h` — grepping for a prototype finds
  nothing, they are macros.  `pci_find_bsf()`/`pci_alloc_msi()` are real.

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
