/*-
 * SPDX-License-Identifier: BSD-3-Clause
 * IPC3 layouts: Copyright(c) 2016-2021 Intel Corporation.
 * Wire structures are provided in vendor/ with their original notices.
 */
#ifdef ESSX_OFFLINE_TEST
#include "tests/offline.h"
#else
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include "essxsof.h"
#endif

CTASSERT(sizeof(struct sof_ipc_comp) == 28);
CTASSERT(sizeof(struct sof_ipc_comp_config) == 36);
CTASSERT(sizeof(struct sof_ipc_comp_host) == 76);
CTASSERT(sizeof(struct sof_ipc_comp_dai) == 80);
CTASSERT(sizeof(struct sof_ipc_stream_params) == 84);
CTASSERT(sizeof(struct sof_ipc_pcm_params) == 108);
CTASSERT(sizeof(struct sof_ipc_dai_ssp_params) == 60);

int
essx_parse_image(const uint8_t *data, size_t size, struct essx_image *image)
{
    size_t full, pos, len, ws;
    const uint8_t *w, *e;
    uint64_t addr;
    uint32_t count, type, id, bytes, off;
    unsigned i;

    bzero(image, sizeof(*image));
    if (size < 20 || memcmp(data,"XMan",4) != 0)
        return (EINVAL);
    full=le32dec(data+4); pos=le32dec(data+8);
    if (pos<16 || pos>full || full>size || size-full<4096 ||
        size-full>524288 || (size-full)%128 || memcmp(data+full,"$CPD",4))
        return (EINVAL);
    image->payload=data+full; image->payload_size=size-full;
    while (full-pos>=8) {
        len=le32dec(data+pos+4);
        if (len<8 || len>full-pos) return (EINVAL);
        if (le32dec(data+pos)!=1) { pos+=len; continue; }
        if (len<24) return (EINVAL);
        w=data+pos+8; ws=le32dec(w); count=le32dec(w+12);
        if (ws<16 || ws>len-8 || le32dec(w+8)!=1 || !count || count>16 ||
            count>(ws-16)/24) return (EINVAL);
        for (i=0;i<count;i++) {
            e=w+16+i*24; type=le32dec(e+4); id=le32dec(e+8);
            bytes=le32dec(e+16); off=le32dec(e+20);
            addr=0x80000ULL+id*0x20000ULL+off;
            if (off>=0x20000 || !bytes || bytes>0x20000-off ||
                addr+bytes>0x100000 || (addr&3)) return (EINVAL);
            if (type==0) {
                if (image->host_size) return (EINVAL);
                image->host=(uint32_t)addr; image->host_size=bytes;
            } else if (type==1) {
                if (image->dsp_size) return (EINVAL);
                image->dsp=(uint32_t)addr; image->dsp_size=bytes;
            }
        }
        if (image->host_size<1024 || image->dsp_size<108 ||
            !((uint64_t)image->host+image->host_size<=image->dsp ||
            (uint64_t)image->dsp+image->dsp_size<=image->host)) return (EINVAL);
        return (0);
    }
    return (EINVAL);
}

static void
comp_header(struct sof_ipc_comp *c, size_t size, uint32_t id, uint32_t type)
{
    c->hdr.size=size; c->hdr.cmd=SOF_IPC_GLB_TPLG_MSG|SOF_IPC_TPLG_COMP_NEW;
    c->id=id; c->type=type; c->pipeline_id=1;
}

static void
comp_config(struct sof_ipc_comp_config *c)
{
    c->hdr.size=sizeof(*c); c->periods_sink=2; c->periods_source=2;
    c->frame_fmt=SOF_IPC_FRAME_S24_4LE; c->xrun_action=SOF_XRUN_STOP;
}

int
essx_stream_command(struct essx_softc *sc, uint32_t cmd, const char *label)
{
    struct sof_ipc_stream msg={ .hdr={sizeof(msg),SOF_IPC_GLB_STREAM_MSG|cmd},
        .comp_id=ESSX_HOST_ID };
    return (essx_ipc(sc,&msg,sizeof(msg),label));
}

/* Minimal fixed-rate host -> buffer -> SSP0 path. No vendor NHLT blob writes. */
int
essx_make_topology(struct essx_softc *sc)
{
    struct sof_ipc_comp_host host;
    struct sof_ipc_buffer buffer;
    struct sof_ipc_comp_dai dai;
    struct sof_ipc_dai_config config;
    struct sof_ipc_pipe_new pipe;
    struct sof_ipc_pipe_comp_connect connect;
    struct sof_ipc_pipe_ready ready;
    struct sof_ipc_pcm_params params;
    int error;
#define SEND(m,l) do { error=essx_ipc(sc,&(m),sizeof(m),(l)); if(error) return(error); } while(0)
    bzero(&host,sizeof(host)); comp_header(&host.comp,sizeof(host),ESSX_HOST_ID,SOF_COMP_HOST);
    comp_config(&host.config); host.direction=SOF_IPC_STREAM_PLAYBACK; host.no_irq=1;
    SEND(host,"HOST_NEW");
    bzero(&buffer,sizeof(buffer)); comp_header(&buffer.comp,sizeof(buffer),ESSX_BUFFER_ID,SOF_COMP_BUFFER);
    buffer.comp.hdr.cmd=SOF_IPC_GLB_TPLG_MSG|SOF_IPC_TPLG_BUFFER_NEW;
    buffer.size=768; buffer.caps=SOF_MEM_CAPS_RAM;
    SEND(buffer,"BUFFER_NEW");
    bzero(&dai,sizeof(dai)); comp_header(&dai.comp,sizeof(dai),ESSX_DAI_ID,SOF_COMP_DAI);
    comp_config(&dai.config); dai.direction=SOF_IPC_STREAM_PLAYBACK; dai.type=SOF_DAI_INTEL_SSP;
    SEND(dai,"DAI_NEW");
    bzero(&pipe,sizeof(pipe)); pipe.hdr.size=sizeof(pipe);
    pipe.hdr.cmd=SOF_IPC_GLB_TPLG_MSG|SOF_IPC_TPLG_PIPE_NEW;
    pipe.comp_id=ESSX_PIPE_ID; pipe.pipeline_id=1; pipe.sched_id=ESSX_DAI_ID;
    pipe.period=1000; pipe.frames_per_sched=48; pipe.time_domain=SOF_TIME_DOMAIN_TIMER;
    pipe.xrun_limit_usecs=1000;
    SEND(pipe,"PIPE_NEW");
    bzero(&connect,sizeof(connect)); connect.hdr.size=sizeof(connect);
    connect.hdr.cmd=SOF_IPC_GLB_TPLG_MSG|SOF_IPC_TPLG_COMP_CONNECT;
    connect.source_id=ESSX_HOST_ID; connect.sink_id=ESSX_BUFFER_ID;
    SEND(connect,"CONNECT_HOST_BUFFER");
    connect.source_id=ESSX_BUFFER_ID; connect.sink_id=ESSX_DAI_ID;
    SEND(connect,"CONNECT_BUFFER_DAI");
    bzero(&config,sizeof(config)); config.hdr.size=sizeof(config);
    config.hdr.cmd=SOF_IPC_GLB_DAI_MSG|SOF_IPC_DAI_CONFIG;
    config.type=SOF_DAI_INTEL_SSP; config.format=SOF_DAI_FMT_I2S|SOF_DAI_FMT_CBC_CFC;
    config.flags=SOF_DAI_CONFIG_FLAGS_HW_PARAMS;
    config.ssp.mclk_rate=19200000; config.ssp.bclk_rate=3072000;
    config.ssp.fsync_rate=48000; config.ssp.tdm_slots=2;
    config.ssp.rx_slots=3; config.ssp.tx_slots=3;
    config.ssp.sample_valid_bits=24; config.ssp.tdm_slot_width=32;
    config.ssp.clks_control=SOF_DAI_INTEL_SSP_CLKCTRL_MCLK_ES;
    SEND(config,"SSP0_CONFIG");
    bzero(&ready,sizeof(ready)); ready.hdr.size=sizeof(ready);
    ready.hdr.cmd=SOF_IPC_GLB_TPLG_MSG|SOF_IPC_TPLG_PIPE_COMPLETE;
    ready.comp_id=ESSX_PIPE_ID;
    SEND(ready,"PIPE_COMPLETE");
    bzero(&params,sizeof(params)); params.hdr.size=sizeof(params);
    params.hdr.cmd=SOF_IPC_GLB_STREAM_MSG|SOF_IPC_STREAM_PCM_PARAMS;
    params.comp_id=ESSX_HOST_ID; params.flags=SOF_PCM_FLAG_XRUN_STOP;
    params.params.hdr.size=sizeof(params.params);
    params.params.buffer.hdr.size=sizeof(params.params.buffer);
    params.params.buffer.size=ESSX_AUDIO_SIZE;
    params.params.frame_fmt=SOF_IPC_FRAME_S24_4LE; params.params.rate=48000;
    params.params.stream_tag=1; params.params.channels=2;
    params.params.sample_valid_bytes=3; params.params.sample_container_bytes=4;
    params.params.no_stream_position=1; params.params.host_period_bytes=4096;
    params.params.chmap[0]=0; params.params.chmap[1]=1;
    SEND(params,"PCM_PARAMS");
#undef SEND
    return (0);
}
