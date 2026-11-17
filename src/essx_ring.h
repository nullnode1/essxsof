/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSX_RING_H
#define ESSX_RING_H
/* S32_LE PCM frontend -> signed S24-in-32 SOF samples at unity gain.
 * Preserve the top 24 sample bits; no extra fixed 1/4 attenuation.
 * The codec's existing -12 dB attenuation remains in place. */
static inline uint32_t
essx_s32_to_s24(uint32_t sample)
{
    return((sample>>8)|((sample&0x80000000u)?0xff000000u:0));
}
static inline uint32_t
essx_ring_pos(uint32_t physical,uint32_t base)
{
    return((physical+ESSX_AUDIO_SIZE-base)%ESSX_AUDIO_SIZE)&~7u;
}
static inline int
essx_ring_copy(uint8_t *dest,const uint8_t *source,uint32_t base)
{
    uint32_t i,at=base,v;
    if(base>=ESSX_AUDIO_SIZE || (base&7u))return(EINVAL);
    for(i=0;i<ESSX_AUDIO_SIZE;i+=4) {
        v=(uint32_t)source[i]|(uint32_t)source[i+1]<<8|
            (uint32_t)source[i+2]<<16|(uint32_t)source[i+3]<<24;
        v=essx_s32_to_s24(v);
        dest[at]=(uint8_t)v;dest[at+1]=(uint8_t)(v>>8);
        dest[at+2]=(uint8_t)(v>>16);dest[at+3]=(uint8_t)(v>>24);
        at+=4;if(at==ESSX_AUDIO_SIZE)at=0;
    }
    return(0);
}
#endif
