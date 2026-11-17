SYSDIR?= /usr/src/sys
KMOD= essxsof
SRCS= essxsof.c essx_ipc.c essx_codec.c essx_pcm.c
SRCS+= opt_acpi.h acpi_if.h bus_if.h device_if.h pci_if.h channel_if.h mixer_if.h
CFLAGS+= -I${.CURDIR}/vendor

.include <bsd.kmod.mk>
