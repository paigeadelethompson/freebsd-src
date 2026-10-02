/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 Paige Adele Thompson
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * 16550-compatible UART backend for the Surface Serial Hub transport.
 * The UART is found via an ACPI UARTSerialBus resource on the SAM serial
 * hub device (MSHW0084); FreeBSD has no serdev bus, so this driver owns
 * the port directly instead of leaving it to uart(4).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/kernel.h>
#include <sys/libkern.h>
#include <sys/lock.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/cpufunc.h>
#include <machine/resource.h>

#include "surface_sam.h"
#include "surface_sam_uart.h"

/* 16550 register offsets (before register shift). */
#define	UART_RBR	0x0	/* R: receive buffer (DLAB=0) */
#define	UART_THR	0x0	/* W: transmit holding (DLAB=0) */
#define	UART_DLL	0x0	/* R/W: divisor latch low (DLAB=1) */
#define	UART_IER	0x1	/* W: interrupt enable (DLAB=0) */
#define	UART_DLM	0x1	/* R/W: divisor latch high (DLAB=1) */
#define	UART_IIR	0x2	/* R: interrupt identification */
#define	UART_FCR	0x2	/* W: FIFO control */
#define	UART_LCR	0x3	/* R/W: line control */
#define	UART_MCR	0x4	/* R/W: modem control */
#define	UART_LSR	0x5	/* R: line status */
#define	UART_MSR	0x6	/* R: modem status */
#define	UART_SCR	0x7	/* R/W: scratch */

#define	UART_IER_ERBFI		0x01	/* enable received data int */

#define	UART_IIR_NOPEND		0x01	/* no interrupt pending */
#define	UART_IIR_IDMASK		0x0e	/* interrupt ID (bits 3:1) */
#define	UART_IIR_MSI		0x00	/* modem status */
#define	UART_IIR_THRE		0x02	/* THR empty */
#define	UART_IIR_RDA		0x04	/* data available */
#define	UART_IIR_LSR		0x06	/* line status */
#define	UART_IIR_RXTO		0x0c	/* character timeout */

#define	UART_FCR_ENABLE		0x01
#define	UART_FCR_RXCLR		0x02
#define	UART_FCR_TXCLR		0x04

#define	UART_LCR_DLAB		0x80

#define	UART_MCR_DTR		0x01
#define	UART_MCR_RTS		0x02
#define	UART_MCR_OUT2		0x08	/* gates IRQ output on PC UARTs */
#define	UART_MCR_AFE		0x20	/* auto flow control (16750+) */

#define	UART_LSR_DR		0x01	/* data ready */
#define	UART_LSR_THRE		0x20	/* THR empty */

#define	SAM_UART_RCLK_DEFAULT	1843200
#define	SAM_UART_BAUD_DEFAULT	115200

/*
 * Upper bound on how long a transmit may wait for the transmitter.  A
 * port that never drains has to fail the request instead of blocking its
 * caller: the parser acknowledges frames by transmitting, and that runs on
 * the transport's taskqueue thread, so a stuck port would otherwise hold
 * up every request behind it.
 */
#define	SAM_UART_TX_TIMEOUT_MS	500

/*
 * Intel LPSS (Sunrisepoint and later, which includes the Surface Laptop 4
 * UARTs) maps the functional UART registers and a private block of
 * "additional registers" into one 4 KB BAR: functional at offset 0,
 * additional registers at 0x200.  The offsets and field positions below
 * follow the PCH documentation of that block.  It has to be put in order
 * before a single functional register answers, because the functional
 * clock is off and the functional block is held in reset: until that is
 * undone every functional register reads back 0xff.
 */
#define	SAM_LPSS_PRIV		0x200	/* start of the additional registers */
#define	SAM_LPSS_CLK		0x00	/* clock ratio: M in 15:1, N in 31:16 */
#define	SAM_LPSS_CLK_ENABLE	0x00000001	/* gates the functional clock */
#define	SAM_LPSS_CLK_UPDATE	0x80000000	/* latches a new ratio */
#define	SAM_LPSS_CLK_MSHIFT	1
#define	SAM_LPSS_CLK_MMASK	0x3fff
#define	SAM_LPSS_CLK_NSHIFT	16
#define	SAM_LPSS_CLK_NMASK	0x3fff
#define	SAM_LPSS_RESET		0x04	/* 1:0 functional block, 2: iDMA */
#define	SAM_LPSS_RESET_FUNC	(3 << 0)
#define	SAM_LPSS_RESET_IDMA	(1 << 2)
#define	SAM_LPSS_REMAP		0x40	/* address the block maps itself to */
#define	SAM_LPSS_CAPS		0xfc	/* 7:4 device type, 1 = uart */
#define	SAM_LPSS_TYPE_UART	1

/*
 * Frequency feeding the additional register block, before the ratio in the
 * register above is applied: 100 MHz on this generation, the figure the
 * reference driver uses for the LPSS UARTs of every SoC since Apollo Lake.
 */
#define	SAM_LPSS_BASE_FREQ	100000000
#define	SAM_DW_DLF		0xc0	/* fractional divisor latch */

struct sam_uart {
	device_t		dev;
	struct resource		*io_res;
	device_t		res_dev;    /* device holding the window */
	int			res_type;   /* SYS_RES_* of the window */
	int			res_rid;
	uint32_t		window;	    /* size of the window in bytes */
	struct resource		*irq_res;
	void			*ih;
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
	uint8_t			shift;
	bool			mem;		/* memory-space window */
	bool			w32;		/* 32-bit register accesses */
	bool			tx_broken;	/* port stopped draining */
	bus_size_t		priv;	    /* lpss additional registers */
	uint32_t		clk_reg;	/* clock register as found */
	uint32_t		base;	    /* clock before its ratio */
	uint32_t		rclk;	    /* clock the port runs from */
	const struct sam_uart_ops *ops;
	void			*arg;
};

static uint8_t
sam_uart_rd(struct sam_uart *uart, uint8_t reg)
{
	bus_size_t off;

	off = (bus_size_t)(reg << uart->shift);
	if (uart->w32)
		return (bus_space_read_4(uart->bst, uart->bsh, off) & 0xff);
	return (bus_space_read_1(uart->bst, uart->bsh, off));
}

static void
sam_uart_wr(struct sam_uart *uart, uint8_t reg, uint8_t val)
{
	bus_size_t off;

	off = (bus_size_t)(reg << uart->shift);
	if (uart->w32)
		bus_space_write_4(uart->bst, uart->bsh, off, val);
	else
		bus_space_write_1(uart->bst, uart->bsh, off, val);
}

/*
 * Claim an interrupt line for a device, adding it to the resource list
 * first if it is not there yet.  The line's trigger and polarity are
 * configured by whatever routed it.
 */
static struct resource *
sam_uart_irq_alloc(device_t dev, uint64_t irq)
{
	struct resource *res;

	res = bus_alloc_resource(dev, SYS_RES_IRQ, 0, irq, irq, 1,
	    RF_ACTIVE | RF_SHAREABLE);
	if (res == NULL) {
		(void)bus_set_resource(dev, SYS_RES_IRQ, 0, irq, 1);
		res = bus_alloc_resource(dev, SYS_RES_IRQ, 0, irq, irq, 1,
		    RF_ACTIVE | RF_SHAREABLE);
	}
	return (res);
}

/*
 * Greatest common divisor, for reducing a clock ratio to its smallest
 * numerator and denominator.
 */
static uint64_t
sam_uart_gcd(uint64_t a, uint64_t b)
{

	while (b != 0) {
		uint64_t t;

		t = a % b;
		a = b;
		b = t;
	}
	return (a);
}

/*
 * Read the clock the functional block runs from: the ratio in the clock
 * register scales the base frequency.  A zero ratio field means the ratio
 * is not programmed and the base frequency is used unchanged.
 */
static uint32_t
sam_uart_lpss_clock(struct sam_uart *uart)
{
	uint32_t reg, m, n;

	reg = bus_space_read_4(uart->bst, uart->bsh, uart->priv + SAM_LPSS_CLK);
	m = (reg >> SAM_LPSS_CLK_MSHIFT) & SAM_LPSS_CLK_MMASK;
	n = (reg >> SAM_LPSS_CLK_NSHIFT) & SAM_LPSS_CLK_NMASK;
	if (m == 0 || n == 0)
		return (uart->base);
	return ((uint32_t)((uint64_t)uart->base * m / n));
}

/*
 * Ask the controller for a clock of the given frequency by programming its
 * ratio.  The register is read back afterwards, because the layout of
 * these fields is documented one way and has been implemented differently
 * across generations: if the ratio does not take, the value that was in
 * the register before is put back, so that a wrong guess cannot leave the
 * block running off a broken clock.
 */
static bool
sam_uart_lpss_set_clock(struct sam_uart *uart, uint32_t target)
{
	bus_size_t off;
	uint64_t gcd;
	uint32_t want, m, n;

	gcd = sam_uart_gcd(target, uart->base);
	m = (uint32_t)(target / gcd);
	n = (uint32_t)(uart->base / gcd);
	if (m > SAM_LPSS_CLK_MMASK || n > SAM_LPSS_CLK_NMASK) {
		device_printf(uart->dev, "clock ratio %u/%u for %u Hz does not "
		    "fit in the register\n", m, n, target);
		return (false);
	}

	off = uart->priv + SAM_LPSS_CLK;
	want = (n << SAM_LPSS_CLK_NSHIFT) | (m << SAM_LPSS_CLK_MSHIFT) |
	    SAM_LPSS_CLK_ENABLE;
	bus_space_write_4(uart->bst, uart->bsh, off, want);
	if (sam_uart_lpss_clock(uart) == target) {
		uart->rclk = target;
		device_printf(uart->dev, "clock set to %u Hz (ratio %u/%u)\n",
		    target, m, n);
		return (true);
	}

	/* Some parts latch a new ratio only once an update bit is set. */
	bus_space_write_4(uart->bst, uart->bsh, off,
	    want | SAM_LPSS_CLK_UPDATE);
	if (sam_uart_lpss_clock(uart) == target) {
		uart->rclk = target;
		device_printf(uart->dev, "clock set to %u Hz (ratio %u/%u,"
		    " after update)\n", target, m, n);
		return (true);
	}

	/* Put back what was in there and carry on with the clock we had. */
	bus_space_write_4(uart->bst, uart->bsh, off, uart->clk_reg);
	uart->rclk = sam_uart_lpss_clock(uart);
	device_printf(uart->dev, "clock register %#x did not take ratio "
	    "%u/%u, restored %#x, clock stays at %u Hz\n", want, m, n,
	    uart->clk_reg, uart->rclk);
	return (false);
}

/*
 * Put an Intel LPSS UART in order: take the functional block out of reset
 * and tell the block where its own registers live.  Both are needed before a
 * functional register answers anything.  On the way, work out what the
 * block runs its clock from so that the divisor can be programmed for the
 * rate the controller really runs at rather than an assumed one.
 * Returns true if this window is an LPSS UART and it is now out of reset.
 */
static bool
sam_uart_lpss_init(struct sam_uart *uart, uint32_t window)
{
	bus_size_t priv;
	uint32_t caps;

	if (!uart->mem || window < SAM_LPSS_PRIV + SAM_LPSS_CAPS + 4)
		return (false);
	priv = SAM_LPSS_PRIV;
	caps = bus_space_read_4(uart->bst, uart->bsh, priv + SAM_LPSS_CAPS);
	if (((caps >> 4) & 0xf) != SAM_LPSS_TYPE_UART) {
		if (bootverbose)
			device_printf(uart->dev,
			    "not an lpss uart (caps %#x)\n", caps);
		return (false);
	}

	/* Reset pulse, then release the functional and iDMA blocks. */
	bus_space_write_4(uart->bst, uart->bsh, priv + SAM_LPSS_RESET, 0x00);
	bus_space_write_4(uart->bst, uart->bsh, priv + SAM_LPSS_RESET,
	    SAM_LPSS_RESET_FUNC | SAM_LPSS_RESET_IDMA);
	bus_space_write_4(uart->bst, uart->bsh, priv + SAM_LPSS_REMAP,
	    (uint32_t)rman_get_start(uart->io_res));

	/*
	 * The clock register scales the base frequency through its ratio
	 * fields and gates the functional clock in bit 0, which the firmware
	 * leaves off.  Open the gate without touching the ratio.
	 */
	uart->priv = priv;
	uart->clk_reg = bus_space_read_4(uart->bst, uart->bsh,
	    priv + SAM_LPSS_CLK);
	device_printf(uart->dev, "lpss clock register %#x, base %u Hz\n",
	    uart->clk_reg, uart->base);
	if ((uart->clk_reg & SAM_LPSS_CLK_ENABLE) == 0)
		bus_space_write_4(uart->bst, uart->bsh, priv + SAM_LPSS_CLK,
		    uart->clk_reg | SAM_LPSS_CLK_ENABLE);
	uart->rclk = sam_uart_lpss_clock(uart);

	device_printf(uart->dev,
	    "lpss uart out of reset (caps %#x, resets %#x, clock %u Hz)\n",
	    caps, bus_space_read_4(uart->bst, uart->bsh, priv + SAM_LPSS_RESET),
	    uart->rclk);
	return (true);
}

static void
sam_uart_drain(struct sam_uart *uart)
{
	uint8_t buf[64];
	size_t n;
	int guard;

	n = 0;
	for (guard = 0; guard < 4096; guard++) {
		if ((sam_uart_rd(uart, UART_LSR) & UART_LSR_DR) == 0)
			break;
		buf[n++] = sam_uart_rd(uart, UART_RBR);
		if (n == sizeof(buf)) {
			uart->ops->input(uart->arg, buf, n);
			n = 0;
		}
	}
	if (n > 0)
		uart->ops->input(uart->arg, buf, n);
}

static void
sam_uart_intr(void *arg)
{
	struct sam_uart *uart;
	int guard;

	uart = arg;
	for (guard = 0; guard < 64; guard++) {
		uint8_t iir;

		iir = sam_uart_rd(uart, UART_IIR);
		if (iir & UART_IIR_NOPEND)
			break;
		switch (iir & UART_IIR_IDMASK) {
		case UART_IIR_RDA:
		case UART_IIR_RXTO:
			sam_uart_drain(uart);
			break;
		case UART_IIR_LSR:
			(void)sam_uart_rd(uart, UART_LSR);
			sam_uart_drain(uart);
			break;
		case UART_IIR_MSI:
			(void)sam_uart_rd(uart, UART_MSR);
			break;
		default:
			/*
			 * Transmitter interrupts are never enabled; drain
			 * once more and stop rather than spin on a source
			 * we cannot clear.
			 */
			sam_uart_drain(uart);
			return;
		}
	}
}

static int
sam_uart_wait_thre(struct sam_uart *uart)
{
	int i;

	/*
	 * Bounded by an iteration count rather than by the clock: this runs
	 * while the kernel is still coming up, where the clock does not move
	 * and a clock based bound would never expire.
	 */
	for (i = 0; i < SAM_UART_TX_TIMEOUT_MS * 100; i++) {
		if ((sam_uart_rd(uart, UART_LSR) & UART_LSR_THRE) != 0)
			return (0);
		DELAY(10);
	}
	uart->tx_broken = true;
	return (EIO);
}

int
sam_uart_tx(struct sam_uart *uart, const uint8_t *buf, size_t len)
{
	size_t i;
	int error;

	/* A port that stopped draining will not start again by itself. */
	if (uart->tx_broken)
		return (EIO);

	for (i = 0; i < len; i++) {
		error = sam_uart_wait_thre(uart);
		if (error != 0)
			return (error);
		sam_uart_wr(uart, UART_THR, buf[i]);
	}
	return (0);
}

/*
 * Find the register layout of the window.  Intel LPSS (DesignWare core)
 * spaces registers four bytes apart and wants 32-bit accesses
 * (reg-shift 2, reg-io-width 4 in the reference intel-lpss driver); a
 * classic 16550 uses byte-wide registers eight bytes apart.  The scratch
 * register round-trips a pattern on both, so test each plausible
 * (access width, register shift) pair and keep the first that answers.
 */
static bool
sam_uart_probe_regs(struct sam_uart *uart, bool w32, uint8_t shift,
    uint32_t window)
{
	bus_size_t scr;
	uint8_t a, b;

	scr = (bus_size_t)UART_SCR << shift;
	if (scr + (w32 ? 4 : 1) > window)
		return (false);
	if (w32) {
		bus_space_write_4(uart->bst, uart->bsh, scr, 0xa5);
		a = bus_space_read_4(uart->bst, uart->bsh, scr) & 0xff;
		bus_space_write_4(uart->bst, uart->bsh, scr, 0x5a);
		b = bus_space_read_4(uart->bst, uart->bsh, scr) & 0xff;
	} else {
		bus_space_write_1(uart->bst, uart->bsh, scr, 0xa5);
		a = bus_space_read_1(uart->bst, uart->bsh, scr);
		bus_space_write_1(uart->bst, uart->bsh, scr, 0x5a);
		b = bus_space_read_1(uart->bst, uart->bsh, scr);
	}
	return (a == 0xa5 && b == 0x5a);
}

static int
sam_uart_hw_init(struct sam_uart *uart, const struct sam_uart_config *cfg)
{
	char div[48];
	uint64_t base, scaled;
	uint32_t baud, rclk, divisor, actual, rem, frac, dlf_old, dlf_size;

	baud = cfg->uart_baud != 0 ? cfg->uart_baud : SAM_UART_BAUD_DEFAULT;
	if (cfg->uart_rclk != 0) {
		rclk = cfg->uart_rclk;		/* tunable override */
	} else if (uart->rclk != 0) {
		rclk = uart->rclk;		/* from the lpss clock ratio */
	} else {
		rclk = SAM_UART_RCLK_DEFAULT;
	}

	/*
	 * A DesignWare UART has a fractional divisor latch next to the
	 * 16550 registers, which is how it reaches a rate that is not an
	 * integer multiple of 16x the baud rate: 4 MBd off a 100 MHz clock
	 * needs a divisor of 1 + 144/256.  Probe how wide that latch is and
	 * use it whenever the clock does not divide out exactly.  Not every
	 * core implements it, in which case the integer divisor below is all
	 * there is - say so, because it leaves the baud rate wrong.
	 */
	dlf_size = 0;
	if (uart->w32 && uart->window > SAM_DW_DLF + 4) {
		dlf_old = bus_space_read_4(uart->bst, uart->bsh, SAM_DW_DLF);
		bus_space_write_4(uart->bst, uart->bsh, SAM_DW_DLF, 0xffffffff);
		frac = bus_space_read_4(uart->bst, uart->bsh, SAM_DW_DLF);
		bus_space_write_4(uart->bst, uart->bsh, SAM_DW_DLF, dlf_old);
		dlf_size = frac != 0 ? fls((int)frac) : 0;
		device_printf(uart->dev,
		    "fractional divisor latch %#x: read %#x with all ones "
		    "written\n", (uint32_t)SAM_DW_DLF, frac);
	}

	base = (uint64_t)baud * 16;
	divisor = rclk / base;
	rem = rclk % base;
	frac = 0;

	/*
	 * A DesignWare UART without the fractional latch can only divide the
	 * clock by a whole number, and this one has no latch (the probe
	 * below says so), so 4 MBd off a 120 MHz clock is 1.875 - not a
	 * divisor.  Rather than run at the wrong rate, ask the controller
	 * for the clock that does divide out, the way the reference driver
	 * does when it sets the baud clock to baud * 16.
	 */
	if (uart->priv != 0 && divisor > 0 && divisor <= 0xffff &&
	    (uint64_t)divisor * base != rclk && baud * 16 <= UINT32_MAX)
		(void)sam_uart_lpss_set_clock(uart, baud * 16);
	if (uart->rclk != 0)
		rclk = uart->rclk;
	divisor = rclk / base;
	rem = rclk % base;

	if (divisor == 0) {
		/*
		 * The reference clock cannot be slower than 16x the baud
		 * rate; assume a clock that divides out exactly and say so
		 * so that it can be corrected via hw.surface_sam.rclk.
		 */
		device_printf(uart->dev,
		    "rclk %u too slow for baud %u, assuming %u\n",
		    rclk, baud, baud * 16);
		rclk = baud * 16;
		divisor = 1;
		rem = 0;
	}
	if (divisor > 0xffff) {
		device_printf(uart->dev, "rclk %u yields invalid divisor\n",
		    rclk);
		return (EINVAL);
	}
	if (dlf_size > 0 && rem != 0) {
		frac = (uint32_t)(((uint64_t)rem << dlf_size) / base);
		scaled = ((uint64_t)divisor << dlf_size) + frac;
		actual = (uint32_t)(((uint64_t)rclk << dlf_size) /
		    (16 * scaled));
	} else {
		dlf_size = 0;
		actual = rclk / (16 * divisor);
	}
	if (dlf_size > 0)
		snprintf(div, sizeof(div), "%u + %u/2^%u", divisor, frac,
		    dlf_size);
	else
		snprintf(div, sizeof(div), "%u", divisor);

	/* Program the port: 8-bit lane, no interrupts, known state. */
	sam_uart_wr(uart, UART_IER, 0x00);
	sam_uart_wr(uart, UART_LCR, cfg->uart_lcr | UART_LCR_DLAB);
	if (dlf_size > 0)
		bus_space_write_4(uart->bst, uart->bsh, SAM_DW_DLF, frac);
	sam_uart_wr(uart, UART_DLL, divisor & 0xff);
	sam_uart_wr(uart, UART_DLM, (divisor >> 8) & 0xff);
	sam_uart_wr(uart, UART_LCR, cfg->uart_lcr);
	sam_uart_wr(uart, UART_FCR, UART_FCR_ENABLE | UART_FCR_RXCLR |
	    UART_FCR_TXCLR);
	sam_uart_wr(uart, UART_MCR, UART_MCR_DTR | UART_MCR_RTS |
	    (uart->irq_res != NULL ? UART_MCR_OUT2 : 0) |
	    (cfg->uart_hwflow ? UART_MCR_AFE : 0));

	device_printf(uart->dev,
	    "uart @ %#jx, %u-bit regs (shift %u), baud %u (div %s of rclk %u "
	    "= %u), %s\n",
	    (uintmax_t)rman_get_start(uart->io_res), uart->w32 ? 32 : 8,
	    uart->shift, baud, div, rclk, actual,
	    cfg->uart_hwflow ? "hwflow" : "no hwflow");

	/*
	 * Without the fractional latch an integer divisor is all there is,
	 * so the port ends up at the wrong rate.  That is worth shouting
	 * about: the link cannot work at all like this, and hw.surface_sam.rclk
	 * is the way to correct the assumed clock.
	 */
	if (actual < baud / 100 || actual > baud + baud / 50)
		device_printf(uart->dev, "warning: baud rate is %u, not %u; "
		    "set hw.surface_sam.rclk so that rclk / (16 * divisor) is "
		    "%u\n", actual, baud, baud);

	return (0);
}

struct sam_uart *
sam_uart_attach(device_t dev, const struct sam_uart_config *cfg,
    const struct sam_uart_ops *ops, void *arg)
{
	static const struct {
		bool	mem;
		bool	w32;
		uint8_t	shift;
	} layouts[] = {
		{ true, true, 2 },	/* LPSS/DesignWare MMIO */
		{ true, false, 2 },	/* DesignWare MMIO, byte access */
		{ true, false, 0 },	/* 16550 in memory space */
		{ false, false, 0 },	/* plain 16550 I/O ports */
		{ false, false, 1 },	/* I/O ports, 16-byte stride */
	};
	struct sam_uart *uart;
	uint32_t window, nports;
	int error, flags;
	u_int i;
	bool found;

	uart = malloc(sizeof(*uart), M_SURFACE_SAM, M_WAITOK | M_ZERO);
	uart->dev = dev;
	uart->ops = ops;
	uart->arg = arg;
	uart->mem = cfg->uart_mem;
	uart->base = cfg->uart_base_freq != 0 ? cfg->uart_base_freq :
	    SAM_LPSS_BASE_FREQ;

	nports = cfg->uart_nports < 8 ? 8 : cfg->uart_nports;

	if (cfg->uart_pcidev != NULL) {
		/*
		 * The window is a BAR on the UART's PCI function; take
		 * it there so that address decoding is enabled and
		 * nobody else can claim it.
		 */
		uart->res_dev = cfg->uart_pcidev;
		uart->res_type = cfg->uart_mem ? SYS_RES_MEMORY :
		    SYS_RES_IOPORT;
		uart->res_rid = cfg->uart_rid;
		uart->io_res = bus_alloc_resource(uart->res_dev,
		    uart->res_type, uart->res_rid, 0, ~0ul, 0, RF_ACTIVE);
		if (uart->io_res == NULL) {
			device_printf(dev,
			    "cannot allocate %s window on %s%s\n",
			    uart->res_type == SYS_RES_MEMORY ?
			    "memory" : "I/O",
			    device_get_nameunit(uart->res_dev),
			    device_get_driver(uart->res_dev) != NULL ?
			    " (busy)" : "");
			goto fail;
		}
	} else {
		/*
		 * The I/O range may already be in the child's resource
		 * list (ACPI parsed a plain IO resource); if it is not,
		 * add it ourselves so that the bus can allocate it.
		 */
		uart->res_dev = dev;
		uart->res_type = SYS_RES_IOPORT;
		uart->res_rid = 0;
		uart->io_res = bus_alloc_resource(dev, SYS_RES_IOPORT, 0,
		    cfg->uart_iobase, cfg->uart_iobase + nports - 1, nports,
		    RF_ACTIVE);
		if (uart->io_res == NULL) {
			(void)bus_set_resource(dev, SYS_RES_IOPORT, 0,
			    cfg->uart_iobase, nports);
			uart->io_res = bus_alloc_resource(dev,
			    SYS_RES_IOPORT, 0, cfg->uart_iobase,
			    cfg->uart_iobase + nports - 1, nports, RF_ACTIVE);
		}
		if (uart->io_res == NULL) {
			device_printf(dev, "cannot allocate I/O range %#jx\n",
			    (uintmax_t)cfg->uart_iobase);
			goto fail;
		}
	}
	uart->bst = rman_get_bustag(uart->io_res);
	uart->bsh = rman_get_bushandle(uart->io_res);
	window = (uint32_t)(rman_get_end(uart->io_res) -
	    rman_get_start(uart->io_res) + 1);
	uart->window = window;

	if (bootverbose)
		device_printf(dev, "window %s %#jx-%#jx (%u bytes)\n",
		    uart->mem ? "memory" : "I/O",
		    (uintmax_t)rman_get_start(uart->io_res),
		    (uintmax_t)rman_get_end(uart->io_res), window);

	/*
	 * Bring the controller up before touching any 16550 register: an
	 * LPSS UART answers nothing at all until its clock gate is open
	 * and the functional block is out of reset.
	 */
	sam_uart_lpss_init(uart, window);

	/* Probe the register layout; fall back to the usual one. */
	if (uart->mem) {
		uart->w32 = true;
		uart->shift = 2;
	} else {
		uart->w32 = false;
		uart->shift = nports >= 16 ? 1 : 0;
	}
	found = false;
	for (i = 0; i < nitems(layouts); i++) {
		if (layouts[i].mem != uart->mem)
			continue;
		if (!sam_uart_probe_regs(uart, layouts[i].w32,
		    layouts[i].shift, window))
			continue;
		uart->w32 = layouts[i].w32;
		uart->shift = layouts[i].shift;
		found = true;
		break;
	}
	if (!found)
		device_printf(dev,
		    "register layout probe failed, assuming %u-bit "
		    "registers, shift %u\n", uart->w32 ? 32 : 8,
		    uart->shift);

	/*
	 * Acquire the interrupt before programming the port.  With the
	 * window on a PCI function the interrupt comes from the bus: it
	 * consults the routing table of the parent bridge whenever the
	 * firmware left the interrupt line register of the function empty,
	 * which is what happens when the firmware routes the interrupt
	 * through ACPI.  An I/O window from _CRS names its interrupt
	 * itself, so that one is used as given.
	 */
	if (cfg->uart_pcidev != NULL) {
		uint64_t irq;
		struct resource *res;

		/*
		 * Ask the PCI bus which line this function uses: it consults
		 * the routing the firmware describes and records the answer in
		 * the function, together with the line's trigger and polarity.
		 *
		 * The line is then claimed on our own device rather than on the
		 * PCI function.  A function with no driver of its own never
		 * joins a devclass and so has no name, and a handler cannot be
		 * attached to a nameless device - the interrupt controller
		 * rejects it.  We are the driver for this link, so the claim
		 * belongs to us.
		 */
		irq = 0;
		res = bus_alloc_resource(cfg->uart_pcidev, SYS_RES_IRQ, 0, 0,
		    ~0ul, 1, RF_ACTIVE | RF_SHAREABLE);
		if (res != NULL) {
			irq = rman_get_start(res);
			bus_release_resource(cfg->uart_pcidev, SYS_RES_IRQ, 0,
			    res);
		}
		if (irq != 0 && irq != 0xff) {
			uart->irq_res = sam_uart_irq_alloc(dev, irq);
			if (uart->irq_res != NULL)
				device_printf(dev, "interrupt %llu, routed by "
				    "the pci bus\n", (unsigned long long)irq);
			else
				device_printf(dev, "cannot claim irq %llu\n",
				    (unsigned long long)irq);
		} else {
			device_printf(dev,
			    "no interrupt routed for the uart\n");
		}
	} else if (cfg->uart_have_irq) {
		if (bootverbose)
			device_printf(dev,
			    "requesting irq %ju (trig %u pol %u)\n",
			    (uintmax_t)cfg->uart_irq, cfg->uart_irq_trigger,
			    cfg->uart_irq_polarity);
		uart->irq_res = sam_uart_irq_alloc(dev, cfg->uart_irq);
		if (uart->irq_res == NULL)
			device_printf(dev, "cannot allocate irq %ju\n",
			    (uintmax_t)cfg->uart_irq);
	}

	/*
	 * No interrupt means no transport: the link runs at 4 MBd, which a
	 * once-per-tick poll cannot drain (about 500 bytes arrive per tick
	 * against a 64 byte receive FIFO), so anything at all would overrun
	 * and be lost silently.  Fail here rather than pretend.
	 */
	if (uart->irq_res == NULL) {
		device_printf(dev, "no interrupt routed for the uart\n");
		goto fail_irq;
	}

	error = sam_uart_hw_init(uart, cfg);
	if (error != 0)
		goto fail_irq;

	/*
	 * One INTR_TYPE_* bit, nothing else: intr_priority() only accepts a
	 * single type, and the trigger and polarity of this line were
	 * configured by the interrupt routing that handed us the resource.
	 * Passing them here would not describe the line, it would collide
	 * with the type bits.
	 */
	flags = INTR_TYPE_TTY | INTR_MPSAFE;
	error = bus_setup_intr(dev, uart->irq_res, flags, NULL,
	    sam_uart_intr, uart, &uart->ih);
	if (error != 0) {
		device_printf(dev, "cannot set up the interrupt: %d\n", error);
		goto fail_irq;
	}
	sam_uart_wr(uart, UART_IER, UART_IER_ERBFI);

	return (uart);

fail_irq:
	if (uart->irq_res != NULL)
		bus_release_resource(dev, SYS_RES_IRQ, 0, uart->irq_res);
	if (uart->io_res != NULL)
		bus_release_resource(uart->res_dev, uart->res_type,
		    uart->res_rid, uart->io_res);
fail:
	free(uart, M_SURFACE_SAM);
	return (NULL);
}

void
sam_uart_detach(struct sam_uart *uart)
{

	sam_uart_wr(uart, UART_IER, 0x00);
	if (uart->ih != NULL) {
		bus_teardown_intr(uart->dev, uart->irq_res, uart->ih);
		uart->ih = NULL;
	}
	if (uart->irq_res != NULL)
		bus_release_resource(uart->dev, SYS_RES_IRQ, 0, uart->irq_res);
	if (uart->io_res != NULL)
		bus_release_resource(uart->res_dev, uart->res_type,
		    uart->res_rid, uart->io_res);
	free(uart, M_SURFACE_SAM);
}
