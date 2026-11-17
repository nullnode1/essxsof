/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSX_POSITION_H
#define ESSX_POSITION_H

/* Intel SOF HDA playback uses the per-stream DPIB, not link LPIB. */
#define ESSX_DPIB_BASE 0x1084u
#define ESSX_DPIB_STRIDE 0x20u
#define ESSX_DPIB7 (ESSX_DPIB_BASE+7u*ESSX_DPIB_STRIDE)

enum essx_sample_result {
    ESSX_SAMPLE_OK, ESSX_SAMPLE_INVALID, ESSX_SAMPLE_DESCRIPTOR,
    ESSX_SAMPLE_FIFO, ESSX_SAMPLE_STOPPED
};
struct essx_playback_sample { uint32_t ctl, dpib, lpib; uint8_t status; };

static inline enum essx_sample_result
essx_check_sample(const struct essx_playback_sample *s,uint32_t *position)
{
    if(s->ctl==UINT32_MAX || s->status==UINT8_MAX || s->dpib==UINT32_MAX)
        return(ESSX_SAMPLE_INVALID);
    if(s->status&0x10)return(ESSX_SAMPLE_DESCRIPTOR);
    if(s->status&0x08)return(ESSX_SAMPLE_FIFO);
    if(!(s->ctl&2u))return(ESSX_SAMPLE_STOPPED);
    /* Match the SOF HDA boundary behavior; an invalid link LPIB is irrelevant. */
    *position=s->dpib>=ESSX_AUDIO_SIZE?0:s->dpib;
    return(ESSX_SAMPLE_OK);
}
#endif
