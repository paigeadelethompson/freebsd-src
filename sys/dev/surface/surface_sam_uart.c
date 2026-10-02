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

#define	SAM_UART_TIMEOUT	(hz * 2)	/* per-byte TX stall limit */

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

struct sam_uart {
	device_t		dev;
	struct resource		*io_res;
	device_t		res_dev;    /* device holding the window */
	int			res_type;   /* SYS_RES_* of the window */
	int			res_rid;
	struct resource		*irq_res;
	void			*ih;
	bus_space_tag_t		bst;
	bus_space_handle_t	bsh;
	uint8_t			shift;
	bool			mem;		/* memory-space window */
	bool			w32;		/* 32-bit register accesses */
	bool			polled;		/* no IRQ: callout polling */
	struct callout		poll_ch;
	const struct sam_uart_ops *ops;
	void			*arg;
};

static void sam_uart_poll(void *arg);

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

static void
sam_uart_poll(void *arg)
{
	struct sam_uart *uart;

	uart = arg;
	if ((sam_uart_rd(uart, UART_LSR) & UART_LSR_DR) != 0)
		sam_uart_drain(uart);
	callout_reset(&uart->poll_ch, 1, sam_uart_poll, uart);
}

static int
sam_uart_wait_thre(struct sam_uart *uart)
{
	u_int start;

	start = ticks;
	while ((sam_uart_rd(uart, UART_LSR) & UART_LSR_THRE) == 0) {
		if (ticks - start >= SAM_UART_TIMEOUT)
			return (EIO);
		DELAY(10);
	}
	return (0);
}

int
sam_uart_tx(struct sam_uart *uart, const uint8_t *buf, size_t len)
{
	size_t i;
	int error;

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
	uint32_t baud, rclk, divisor, actual;

	baud = cfg->uart_baud != 0 ? cfg->uart_baud : SAM_UART_BAUD_DEFAULT;
	rclk = cfg->uart_rclk != 0 ? cfg->uart_rclk : SAM_UART_RCLK_DEFAULT;
	divisor = (rclk + 8 * baud) / (16 * baud);
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
	}
	if (divisor > 0xffff) {
		device_printf(uart->dev, "rclk %u yields invalid divisor\n",
		    rclk);
		return (EINVAL);
	}
	actual = rclk / (16 * divisor);

	/* Program the port: 8-bit lane, no interrupts, known state. */
	sam_uart_wr(uart, UART_IER, 0x00);
	sam_uart_wr(uart, UART_LCR, cfg->uart_lcr | UART_LCR_DLAB);
	sam_uart_wr(uart, UART_DLL, divisor & 0xff);
	sam_uart_wr(uart, UART_DLM, (divisor >> 8) & 0xff);
	sam_uart_wr(uart, UART_LCR, cfg->uart_lcr);
	sam_uart_wr(uart, UART_FCR, UART_FCR_ENABLE | UART_FCR_RXCLR |
	    UART_FCR_TXCLR);
	sam_uart_wr(uart, UART_MCR, UART_MCR_DTR | UART_MCR_RTS |
	    (cfg->uart_have_irq ? UART_MCR_OUT2 : 0) |
	    (cfg->uart_hwflow ? UART_MCR_AFE : 0));

	device_printf(uart->dev,
	    "uart @ %#jx, %u-bit regs (shift %u), baud %u "
	    "(div %u of rclk %u = %u), %s, %s\n",
	    (uintmax_t)cfg->uart_iobase, uart->w32 ? 32 : 8, uart->shift,
	    baud, divisor, rclk, actual,
	    cfg->uart_hwflow ? "hwflow" : "no hwflow",
	    cfg->uart_have_irq ? "irq" : "polled");

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
	callout_init(&uart->poll_ch, 1);

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

	error = sam_uart_hw_init(uart, cfg);
	if (error != 0)
		goto fail_io;

	if (cfg->uart_have_irq) {
		uart->irq_res = bus_alloc_resource(dev, SYS_RES_IRQ, 0,
		    cfg->uart_irq, cfg->uart_irq, 1, RF_ACTIVE);
		if (uart->irq_res == NULL) {
			(void)bus_set_resource(dev, SYS_RES_IRQ, 0,
			    cfg->uart_irq, 1);
			uart->irq_res = bus_alloc_resource(dev, SYS_RES_IRQ,
			    0, cfg->uart_irq, cfg->uart_irq, 1, RF_ACTIVE);
		}
		if (uart->irq_res == NULL)
			uart->irq_res = bus_alloc_resource(dev, SYS_RES_IRQ,
			    0, cfg->uart_irq, cfg->uart_irq, 1,
			    RF_ACTIVE | RF_SHAREABLE);
	}

	if (uart->irq_res != NULL) {
		flags = INTR_TYPE_TTY | INTR_MPSAFE;
		/* ACPI_EDGE_SENSITIVE / ACPI_LEVEL_SENSITIVE */
		if (cfg->uart_irq_trigger == 0x01)
			flags |= INTR_TRIGGER_EDGE;
		else if (cfg->uart_irq_trigger == 0x00)
			flags |= INTR_TRIGGER_LEVEL;
		/* ACPI_ACTIVE_LOW / ACPI_ACTIVE_HIGH */
		if (cfg->uart_irq_polarity == 0x01)
			flags |= INTR_POLARITY_LOW;
		else if (cfg->uart_irq_polarity == 0x00)
			flags |= INTR_POLARITY_HIGH;
		error = bus_setup_intr(dev, uart->irq_res, flags, NULL,
		    sam_uart_intr, uart, &uart->ih);
		if (error != 0) {
			device_printf(dev, "bus_setup_intr failed: %d\n",
			    error);
			uart->ih = NULL;
		}
	}

	if (uart->ih == NULL) {
		/* No usable interrupt: poll once per tick. */
		uart->polled = true;
		device_printf(dev,
		    "no interrupt available, falling back to polling\n");
		callout_reset(&uart->poll_ch, 1, sam_uart_poll, uart);
	} else {
		sam_uart_wr(uart, UART_IER, UART_IER_ERBFI);
	}

	return (uart);

fail_io:
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
	if (uart->polled)
		callout_drain(&uart->poll_ch);
	if (uart->irq_res != NULL)
		bus_release_resource(uart->dev, SYS_RES_IRQ, 0,
		    uart->irq_res);
	if (uart->io_res != NULL)
		bus_release_resource(uart->res_dev, uart->res_type,
		    uart->res_rid, uart->io_res);
	free(uart, M_SURFACE_SAM);
}
