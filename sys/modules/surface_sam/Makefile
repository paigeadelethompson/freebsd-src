.PATH: ${SRCTOP}/sys/dev/surface

KMOD=	surface_sam
SRCS=	surface_sam.c surface_sam.h
SRCS+=	surface_sam_uart.c surface_sam_uart.h
SRCS+=	surface_hid.c
SRCS+=	surface_profile.c surface_mon.c surface_rtc.c
SRCS+=	acpi_if.h device_if.h bus_if.h hid_if.h clock_if.h opt_acpi.h

.include <bsd.kmod.mk>
