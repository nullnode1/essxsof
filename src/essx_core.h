/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSX_CORE_H
#define ESSX_CORE_H
#ifdef _KERNEL
#include <sys/errno.h>
#include <sys/stdint.h>
#else
#include <errno.h>
#include <stdint.h>
#endif

/* Tiger Lake: only core 0 is host-managed (Linux SOF tgl_chip_info). */
#define ESSX_CORE_RESET 0x00000001u
#define ESSX_CORE_STALL 0x00000100u
#define ESSX_CORE_SPA   0x00010000u
#define ESSX_CORE_CPA   0x01000000u

struct essx_core_io {
    void *cookie;
    uint32_t (*read)(void *);
    void (*write)(void *, uint32_t);
    void (*delay)(void *, unsigned);
};

static inline int
essx_core_wait(const struct essx_core_io *io, uint32_t mask, uint32_t want)
{
    unsigned us;
    uint32_t v;
    for(us=0;us<50000;us+=10) {
        v=io->read(io->cookie);
        if(v==UINT32_MAX)return(EIO);
        if((v&mask)==want)return(0);
        io->delay(io->cookie,10);
    }
    return(ETIMEDOUT);
}

/* Off-state validation concerns CPA0; other cores are firmware-managed.
 * Reset/stall and reserved fields are not a powered-down core's handshake.
 * Call only with valid PCI mappings and the controller in D0. */
static inline int
essx_core_off(uint32_t v)
{
    return(v!=UINT32_MAX && !(v&ESSX_CORE_CPA)?0:EIO);
}

static inline int
essx_core_stop(const struct essx_core_io *io)
{
    uint32_t v=io->read(io->cookie);
    int error;
    if(v==UINT32_MAX)return(EIO);
    if(!(v&(ESSX_CORE_SPA|ESSX_CORE_CPA)))return(0);

    /* Stall, then assert reset and await its acknowledgement BEFORE SPA0
     * is cleared. Preserve every other core's control bits. */
    io->write(io->cookie,v|ESSX_CORE_STALL);
    v=io->read(io->cookie);if(v==UINT32_MAX)return(EIO);
    io->write(io->cookie,v|ESSX_CORE_RESET);
    error=essx_core_wait(io,ESSX_CORE_RESET,ESSX_CORE_RESET);
    if(error)return(error);
    v=io->read(io->cookie);if(v==UINT32_MAX)return(EIO);
    io->write(io->cookie,v&~ESSX_CORE_SPA);
    return(essx_core_wait(io,ESSX_CORE_CPA,0));
}
#endif
