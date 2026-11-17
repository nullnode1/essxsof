/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSX_WIRE_H
#define ESSX_WIRE_H
#define ESSX_AUDIO_SIZE 49152u
#define ESSX_MEM_SIZE (1024u * 1024u)
#define ESSX_CAP_DATA 0u
#define ESSX_FW_DATA 4096u
#define ESSX_CAP_BDL 0x82000u
#define ESSX_OUT_BDL 0x83000u
#define ESSX_AUDIO_DATA 0x84000u
#define ESSX_DSP_BUSY 0x80000000u
#define ESSX_HOST_ID 1u
#define ESSX_BUFFER_ID 2u
#define ESSX_DAI_ID 3u
#define ESSX_PIPE_ID 4u

struct essx_image {
    const uint8_t *payload;
    size_t payload_size;
    uint32_t host, host_size, dsp, dsp_size;
};
#endif
