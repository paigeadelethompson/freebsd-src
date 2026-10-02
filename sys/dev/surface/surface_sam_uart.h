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
 * Raw 16550-style UART plumbing for the Surface Serial Hub transport.
 * The link layer is deliberately minimal: synchronous transmit, receive
 * handed to a callback, no tty layer in between.
 */

#ifndef _SURFACE_SAM_UART_H_
#define _SURFACE_SAM_UART_H_

#include <sys/types.h>
#include <sys/bus.h>

struct sam_uart;

struct sam_uart_config {
	uint64_t	uart_iobase;	/* I/O port base from ACPI _CRS */
	uint32_t	uart_nports;	/* size of the register window */
	uint64_t	uart_irq;	/* IRQ number (if uart_have_irq) */
	bool		uart_have_irq;
	uint8_t		uart_irq_trigger;   /* ACPI_TRIGGER_* or 0xff */
	uint8_t		uart_irq_polarity;  /* ACPI_ACTIVE_* or 0xff */
	uint32_t	uart_baud;	/* 0 -> 115200 */
	uint32_t	uart_rclk;	/* 0 -> 1843200 */
	uint8_t		uart_lcr;	/* line control word */
	bool		uart_hwflow;	/* RTS/CTS from ACPI */
	/*
	 * Register window behind a PCI function (Intel LPSS UART: the
	 * serial bus descriptor names the controller, the window is a
	 * BAR on it).  uart_pcidev is NULL for a plain I/O window from
	 * _CRS.
	 */
	bool		uart_mem;	/* memory-space window, not I/O */
	device_t	uart_pcidev;	/* PCI device holding the window */
	int		uart_rid;	/* resource id on uart_pcidev */
};

struct sam_uart_ops {
	/* Called from interrupt/poll context with no locks held. */
	void	(*input)(void *arg, const uint8_t *buf, size_t len);
};

struct sam_uart *sam_uart_attach(device_t dev,
	    const struct sam_uart_config *cfg, const struct sam_uart_ops *ops,
	    void *arg);
void	sam_uart_detach(struct sam_uart *uart);
int	sam_uart_tx(struct sam_uart *uart, const uint8_t *buf, size_t len);

#endif /* _SURFACE_SAM_UART_H_ */
