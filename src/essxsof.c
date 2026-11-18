/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Experimental, exclusive-owner Tiger Lake SOF IPC3 playback bring-up.
 * FreeBSD PCM playback through the hardware-verified SOF pipeline.
 * No automatic firmware/GPIO/codec activity at attach; root explicitly enables PCM.
 */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/firmware.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/priv.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <dev/pci/pcireg.h>
#include "bus_if.h"
#include "device_if.h"
#include "pci_if.h"
#include "essxsof.h"
#include "essx_position.h"
#include "essx_core.h"

#define SD0 0x80
#define SD7 0x160
#define ADSPCS 4
#define HIPCTDR 0xc0
#define HIPCTDA 0xc4
#define HIPCIDR 0xd0
#define HIPCIDA 0xd4
#define PPCTL 0x804
#define SPIBCTL 0x704
#define SPIB7 (0x708+8*7)
#define ROM_STATUS 0x80000
#define RESET 1u
#define RUN 2u
#define CORE_SPA ESSX_CORE_SPA
#define CORE_CPA ESSX_CORE_CPA
#define STOPPED 0x101u
CTASSERT(ESSX_FW_DATA+524288<=ESSX_CAP_BDL);
CTASSERT(ESSX_CAP_BDL+4096<=ESSX_OUT_BDL);
CTASSERT(ESSX_OUT_BDL+4096<=ESSX_AUDIO_DATA);
CTASSERT(ESSX_AUDIO_DATA+ESSX_AUDIO_SIZE<=ESSX_MEM_SIZE);

static uint32_t
wait_bits(struct resource *r,bus_size_t offset,uint32_t mask,uint32_t want,unsigned us)
{
    uint32_t v;
    do {
        v=er4(r,offset);
        if(v==UINT32_MAX || (v&mask)==want) return(v);
        DELAY(10);
    } while(us>10 && (us-=10));
    return(v);
}
static int
expect_bits(struct resource *r,bus_size_t off,uint32_t mask,uint32_t want,unsigned us)
{
    uint32_t v=wait_bits(r,off,mask,want,us);
    return(v!=UINT32_MAX&&(v&mask)==want?0:ETIMEDOUT);
}

static void
dma_callback(void *arg,bus_dma_segment_t *segs,int nsegs,int error)
{
    struct essx_dma *d=arg;
    if(!error && nsegs==1 && segs[0].ds_len>=ESSX_MEM_SIZE) {
        d->paddr=segs[0].ds_addr;d->loaded=true;
    }
}
static int
dma_alloc(struct essx_softc *sc)
{
    struct essx_dma *d=&sc->dma;
    int e;
    e=bus_dma_tag_create(bus_get_dma_tag(sc->dev),PAGE_SIZE,0,
        BUS_SPACE_MAXADDR,BUS_SPACE_MAXADDR,NULL,NULL,ESSX_MEM_SIZE,1,
        ESSX_MEM_SIZE,0,NULL,NULL,&d->tag);
    if(e)return(e);
    e=bus_dmamem_alloc(d->tag,&d->vaddr,BUS_DMA_NOWAIT|BUS_DMA_ZERO|BUS_DMA_COHERENT,&d->map);
    if(e)return(e);
    e=bus_dmamap_load(d->tag,d->map,d->vaddr,ESSX_MEM_SIZE,dma_callback,d,BUS_DMA_NOWAIT);
    if(e || !d->loaded)return(e?e:ENOMEM);
    return(0);
}
static void
dma_free(struct essx_softc *sc)
{
    struct essx_dma *d=&sc->dma;
    if(d->loaded) {
        bus_dmamap_sync(d->tag,d->map,BUS_DMASYNC_POSTREAD|BUS_DMASYNC_POSTWRITE);
        bus_dmamap_unload(d->tag,d->map);d->loaded=false;
    }
    if(d->vaddr)bus_dmamem_free(d->tag,d->vaddr,d->map);
    if(d->tag)bus_dma_tag_destroy(d->tag);
    bzero(d,sizeof(*d));
}

static int
stop_dma(struct essx_softc *sc,unsigned off)
{
    uint32_t v=er4(sc->hda,off);
    if(v==UINT32_MAX)return(EIO);
    ew4(sc->hda,off,(v&0xffffffu)&~RUN);
    return(expect_bits(sc->hda,off,RUN,0,50000));
}

static int
hda_reset(struct essx_softc *sc)
{
    int e;
    ew4(sc->hda,0x20,0); /* no host interrupts; DSP traffic is polled */
    bus_write_1(sc->hda,0x4c,0);bus_write_1(sc->hda,0x5c,0);
    ew4(sc->hda,0x08,0);e=expect_bits(sc->hda,8,1,0,50000);
    if(e)return(e);
    DELAY(100);ew4(sc->hda,8,1);e=expect_bits(sc->hda,8,1,1,50000);
    DELAY(1000);return(e);
}

static uint32_t
core_read(void *cookie)
{
    struct essx_softc *sc=cookie;
    return(er4(sc->dsp,ADSPCS));
}
static void
core_write(void *cookie,uint32_t v)
{
    struct essx_softc *sc=cookie;
    ew4(sc->dsp,ADSPCS,v);
}
static void
core_delay(void *cookie,unsigned us)
{
    (void)cookie;
    DELAY(us);
}
static int
host_core_stop(struct essx_softc *sc)
{
    const struct essx_core_io io={sc,core_read,core_write,core_delay};
    int error=essx_core_stop(&io);
    device_printf(sc->dev,"core0 stop: ADSPCS=0x%08x reset-then-power-off=%s errno=%d\n",
        er4(sc->dsp,ADSPCS),error?"failed":"ok",error);
    return(error);
}

static int
setup_stream(struct essx_softc *sc,unsigned sd,unsigned data,unsigned size,
    unsigned bdl_off,uint16_t format,bool playback)
{
    struct essx_bdle *bdl=(struct essx_bdle *)((uint8_t *)sc->dma.vaddr+bdl_off);
    bus_addr_t addr=sc->dma.paddr+data;
    unsigned pos=0,n=0,chunk;
    uint32_t ctl;
    int e;
    e=stop_dma(sc,sd);if(e)return(e);
    ctl=er4(sc->hda,sd)&0x000f0000u;
    ew4(sc->hda,sd,ctl|RESET);e=expect_bits(sc->hda,sd,RESET,RESET,50000);
    if(e)return(e);
    ew4(sc->hda,sd,ctl);e=expect_bits(sc->hda,sd,RESET,0,50000);
    if(e)return(e);
    if(playback)ctl|=1u<<20;
    while(pos<size) {
        chunk=MIN(PAGE_SIZE,size-pos);
        if(size==PAGE_SIZE)chunk=PAGE_SIZE/2;
        if(n>=256)return(EINVAL);
        bdl[n]=(struct essx_bdle){htole32((uint32_t)addr),htole32(addr>>32),
            htole32(chunk),0};
        addr+=chunk;pos+=chunk;n++;
    }
    addr=sc->dma.paddr+bdl_off;
    ew4(sc->hda,sd+8,size);bus_write_2(sc->hda,sd+12,n-1);
    bus_write_2(sc->hda,sd+18,format);
    ew4(sc->hda,sd+24,(uint32_t)addr);ew4(sc->hda,sd+28,addr>>32);
    bus_write_1(sc->hda,sd+3,0x1c);
    ew4(sc->hda,sd,ctl);
    if(er4(sc->hda,sd+8)!=size||er4(sc->hda,sd+24)!=(uint32_t)addr||
        er4(sc->hda,sd+28)!=(uint32_t)(addr>>32))return(EIO);
    return(0);
}

static int
notify_ack(struct essx_softc *sc,uint32_t *command)
{
    uint32_t door=er4(sc->dsp,HIPCTDR);
    if(door==UINT32_MAX)return(EIO);
    if(!(door&ESSX_DSP_BUSY))return(ENOENT);
    *command=er4(sc->dsp,sc->image.dsp+4);
    ew4(sc->dsp,HIPCTDR,ESSX_DSP_BUSY);
    ew4(sc->dsp,HIPCTDA,er4(sc->dsp,HIPCTDA)|ESSX_DSP_BUSY);
    return(0);
}

int
essx_ipc(struct essx_softc *sc,const void *request,size_t length,const char *label)
{
    const uint8_t *p=request;
    uint32_t ack,reqcmd,size,cmd,notification;
    int32_t error;
    unsigned i,us;
    sc->stage=label;
    if(length<8 || length>1024 || length>sc->image.host_size || length%4 ||
        le32dec(p)!=length)return(EINVAL);
    if(expect_bits(sc->dsp,HIPCTDA,ESSX_DSP_BUSY,0,500000) ||
        expect_bits(sc->dsp,HIPCIDR,ESSX_DSP_BUSY,0,500000))return(ETIMEDOUT);
    ack=er4(sc->dsp,HIPCIDA);if(ack==UINT32_MAX)return(EIO);
    if(ack&ESSX_DSP_BUSY) {
        ew4(sc->dsp,HIPCIDA,ESSX_DSP_BUSY);
        if(expect_bits(sc->dsp,HIPCIDA,ESSX_DSP_BUSY,0,50000))return(EIO);
    }
    reqcmd=le32dec(p+4);
    for(i=0;i<length;i+=4)ew4(sc->dsp,sc->image.host+i,le32dec(p+i));
    bus_barrier(sc->dsp,sc->image.host,length,BUS_SPACE_BARRIER_WRITE);
    if(er4(sc->dsp,sc->image.host+4)!=reqcmd)return(EIO);
    ew4(sc->dsp,HIPCIDR,ESSX_DSP_BUSY);
    for(us=0;us<500000;us+=10) {
        ack=er4(sc->dsp,HIPCIDA);
        if(ack==UINT32_MAX)return(EIO);
        if(ack&ESSX_DSP_BUSY)break;
        if(notify_ack(sc,&notification)==0) {
            device_printf(sc->dev,"notification while %s: 0x%08x\n",label,notification);
            /* A streaming XRUN or panic is not a successful transaction. */
            if((notification&0xf0000000u)!=SOF_IPC_FW_READY)return(EIO);
        }
        DELAY(10);
    }
    if(!(ack&ESSX_DSP_BUSY)) {
        device_printf(sc->dev,"IPC %s timeout\n",label);return(ETIMEDOUT);
    }
    size=er4(sc->dsp,sc->image.host);cmd=er4(sc->dsp,sc->image.host+4);
    error=(int32_t)er4(sc->dsp,sc->image.host+8);
    ew4(sc->dsp,HIPCIDA,ESSX_DSP_BUSY);
    device_printf(sc->dev,"IPC %s: cmd=0x%08x size=%u fw_error=%d\n",label,cmd,size,error);
    if(size<12 || size>sc->image.host_size || size%4 ||
        (cmd!=SOF_IPC_GLB_REPLY && cmd!=reqcmd))return(EPROTO);
    return(error==0?0:EIO);
}

static int
boot_firmware(struct essx_softc *sc)
{
    uint32_t v,cmd,abi;
    unsigned port;
    int e;
    sc->phase=1;
    sc->stage="controller-preflight";
    sc->pgctl=pci_read_config(sc->dev,0x44,4);sc->cgctl=pci_read_config(sc->dev,0x48,4);
    sc->em2=er4(sc->hda,0x1030);sc->ltrp=bus_read_1(sc->hda,0x1048);
    if(sc->pgctl==UINT32_MAX||sc->em2==UINT32_MAX)return(EIO);
    sc->saved=true;
    pci_write_config(sc->dev,0x44,sc->pgctl|4,4);
    if(sc->cgctl!=UINT32_MAX)pci_write_config(sc->dev,0x48,sc->cgctl&~2u,4);
    ew4(sc->hda,0x1030,sc->em2&~(1u<<13));
    bus_write_1(sc->hda,0x1048,(sc->ltrp&~0x3f)|(95&0x3f));
    sc->stage="hda-reset";e=hda_reset(sc);if(e)return(e);
    pci_enable_busmaster(sc->dev);
    if(!(pci_read_config(sc->dev,PCIR_COMMAND,2)&PCIM_CMD_BUSMASTEREN))return(EIO);
    /* Re-enable processing after the previous quiesce cleared PPCTL.
     * Like SOF's probe/resume, do this before accessing DSP core control. */
    sc->stage="processing-enable";
    ew4(sc->hda,PPCTL,1u<<30);
    v=er4(sc->hda,PPCTL);
    if(v==UINT32_MAX || !(v&(1u<<30)))return(EIO);
    /* Exclusive PCI ownership does not grant host control of cores 1-3. */
    sc->stage="core-reset";e=host_core_stop(sc);if(e)return(e);
    ew4(sc->hda,PPCTL,(1u<<30)|1u|(1u<<7));
    sc->stage="dma-setup";
    e=setup_stream(sc,SD0,ESSX_CAP_DATA,PAGE_SIZE,ESSX_CAP_BDL,0,false);if(e)return(e);
    e=setup_stream(sc,SD7,ESSX_FW_DATA,sc->image.payload_size,ESSX_OUT_BDL,0x40,true);if(e)return(e);
    ew4(sc->hda,SPIB7,sc->image.payload_size);ew4(sc->hda,SPIBCTL,1u<<7);
    bus_dmamap_sync(sc->dma.tag,sc->dma.map,BUS_DMASYNC_PREREAD|BUS_DMASYNC_PREWRITE);
    sc->stage="capture-start";ew4(sc->hda,SD0,(er4(sc->hda,SD0)&0xffffffu)|RUN);
    e=expect_bits(sc->hda,SD0,RUN|RESET,RUN,50000);if(e)return(e);
    sc->stage="core-power";v=er4(sc->dsp,ADSPCS);if(v==UINT32_MAX)return(EIO);
    ew4(sc->dsp,ADSPCS,v|CORE_SPA|STOPPED);
    sc->powered=true;
    e=expect_bits(sc->dsp,ADSPCS,CORE_CPA,CORE_CPA,50000);if(e)return(e);
    sc->stage="rom-ssp-clock";
    for(port=0;port<3;port++) {
        v=er4(sc->dsp,0x10004+0x1000*port);if(v==UINT32_MAX)return(EIO);
        ew4(sc->dsp,0x10004+0x1000*port,v|(3u<<24));
    }
    sc->stage="rom-ipc-idle";
    if(expect_bits(sc->dsp,HIPCIDR,ESSX_DSP_BUSY,0,50000)||
        expect_bits(sc->dsp,HIPCTDR,ESSX_DSP_BUSY,0,50000))return(EBUSY);
    ew4(sc->dsp,HIPCIDA,ESSX_DSP_BUSY);
    if(expect_bits(sc->dsp,HIPCIDA,ESSX_DSP_BUSY,0,50000))return(EIO);
    ew4(sc->dsp,HIPCIDR,0x81004000);
    v=er4(sc->dsp,ADSPCS);ew4(sc->dsp,ADSPCS,v&~1u);
    e=expect_bits(sc->dsp,ADSPCS,1,0,50000);if(e)return(e);
    v=er4(sc->dsp,ADSPCS);ew4(sc->dsp,ADSPCS,v&~0x100u);
    sc->stage="rom-ipc-ack";
    e=expect_bits(sc->dsp,HIPCIDA,ESSX_DSP_BUSY,ESSX_DSP_BUSY,500000);if(e)return(e);
    ew4(sc->dsp,HIPCIDA,ESSX_DSP_BUSY);
    sc->stage="rom-ready";e=expect_bits(sc->dsp,ROM_STATUS,0xffffff,1,300000);if(e)return(e);
    ew4(sc->hda,SD7,(er4(sc->hda,SD7)&0xffffffu)|RUN);
    sc->stage="firmware-transfer";e=expect_bits(sc->dsp,ROM_STATUS,0xffffff,5,3000000);if(e)return(e);
    e=stop_dma(sc,SD7);if(e)return(e);
    e=stop_dma(sc,SD0);if(e)return(e);
    ew4(sc->hda,SPIBCTL,0);
    sc->stage="FW_READY";e=expect_bits(sc->dsp,HIPCTDR,ESSX_DSP_BUSY,ESSX_DSP_BUSY,2000000);if(e)return(e);
    if(er4(sc->dsp,sc->image.dsp)!=108)return(EPROTO);
    abi=er4(sc->dsp,sc->image.dsp+64);
    e=notify_ack(sc,&cmd);
    if(e || cmd!=SOF_IPC_FW_READY || (abi>>24)!=3)return(EPROTO);
    device_printf(sc->dev,"FW_READY ABI=0x%08x host=0x%x dsp=0x%x\n",abi,sc->image.host,sc->image.dsp);
    sc->phase=2;
    return(0);
}

/* A reset/power transition, not a claim of exact SSP bit restoration. */
static int
mapping_valid(struct essx_softc *sc)
{
    uint64_t bar0,bar4;
    uint32_t cmd=pci_read_config(sc->dev,PCIR_COMMAND,2);
    bar0=((uint64_t)pci_read_config(sc->dev,PCIR_BAR(0)+4,4)<<32)|
        (pci_read_config(sc->dev,PCIR_BAR(0),4)&~0xfu);
    bar4=((uint64_t)pci_read_config(sc->dev,PCIR_BAR(4)+4,4)<<32)|
        (pci_read_config(sc->dev,PCIR_BAR(4),4)&~0xfu);
    if(cmd==0xffff || !(cmd&PCIM_CMD_MEMEN) ||
        bar0!=rman_get_start(sc->hda) || bar4!=rman_get_start(sc->dsp)) {
        device_printf(sc->dev,"PCI mapping mismatch CMD=0x%04x BAR0=0x%jx BAR4=0x%jx\n",
            cmd,(uintmax_t)bar0,(uintmax_t)bar4);
        return(EIO);
    }
    return(0);
}

static int
quiesce(struct essx_softc *sc)
{
    uint32_t v;
    int error=0,e,out_stop,cap_stop;
    if(sc->hda==NULL||sc->dsp==NULL)return(0);
    e=stop_dma(sc,SD7);if(e)error=e;
    e=stop_dma(sc,SD0);if(e&&!error)error=e;
    pci_disable_busmaster(sc->dev);
    v=pci_read_config(sc->dev,PCIR_COMMAND,2);
    if(v==0xffff || (v&PCIM_CMD_BUSMASTEREN)) {
        sc->poisoned=true;
        device_printf(sc->dev,"DMA ownership uncertain; retain resources until reboot\n");
        return(EBUSY);
    }
    if(sc->powered) {
        e=host_core_stop(sc);if(e&&!error)error=e;
    }
    e=hda_reset(sc);if(e)error=e;
    ew4(sc->hda,PPCTL,0);ew4(sc->hda,SPIBCTL,0);
    device_printf(sc->dev,"shutdown: core-before-D3=0x%08x\n",er4(sc->dsp,ADSPCS));
    /* Save with bus mastering disabled, then restore BARs and PCI configuration. */
    pci_save_state(sc->dev);
    e=pci_set_powerstate(sc->dev,PCI_POWERSTATE_D3);if(e)error=e;
    DELAY(10000);
    e=pci_set_powerstate(sc->dev,PCI_POWERSTATE_D0);if(e)error=e;
    DELAY(1000);
    pci_restore_state(sc->dev);
    if(pci_get_powerstate(sc->dev)!=PCI_POWERSTATE_D0) {
        pci_disable_busmaster(sc->dev);sc->poisoned=true;return(EIO);
    }
    pci_disable_busmaster(sc->dev);
    if(pci_read_config(sc->dev,PCIR_COMMAND,2)&PCIM_CMD_BUSMASTEREN) {
        sc->poisoned=true;return(EBUSY);
    }
    e=mapping_valid(sc);
    if(e) {sc->poisoned=true;return(e);}
    /* Sample power acknowledgement while our D0/ungated setup is still
     * present, before restoring the original power/clock-gating policy.
     * Never infer failure from unrelated/reserved powered-off bits. */
    v=er4(sc->dsp,ADSPCS);
    e=essx_core_off(v);if(e&&!error)error=e;
    out_stop=stop_dma(sc,SD7);if(out_stop&&!error)error=out_stop;
    cap_stop=stop_dma(sc,SD0);if(cap_stop&&!error)error=cap_stop;
    device_printf(sc->dev,"shutdown: core-after-D0=0x%08x CPA0-off=%s DMA-stopped=%s\n",
        v,essx_core_off(v)?"no":"yes",out_stop||cap_stop?"failed":"yes");
    if(sc->saved) {
        pci_write_config(sc->dev,0x44,sc->pgctl,4);
        if(sc->cgctl!=UINT32_MAX)pci_write_config(sc->dev,0x48,sc->cgctl,4);
        ew4(sc->hda,0x1030,sc->em2);bus_write_1(sc->hda,0x1048,sc->ltrp);
        if(pci_read_config(sc->dev,0x44,4)!=sc->pgctl ||
            (sc->cgctl!=UINT32_MAX&&pci_read_config(sc->dev,0x48,4)!=sc->cgctl)||
            er4(sc->hda,0x1030)!=sc->em2||bus_read_1(sc->hda,0x1048)!=sc->ltrp)error=EIO;
    }
    device_printf(sc->dev,"shutdown: DMA-disabled=yes quiesce=%s errno=%d\n",
        error?"failed":"ok",error);
    sc->powered=false;sc->saved=false;
    if(error)sc->poisoned=true;
    return(error);
}

int
essx_pcm_position(struct essx_softc *sc,uint32_t *position)
{
    struct essx_playback_sample sample;
    uint32_t notification;
    enum essx_sample_result result;
    sample.ctl=er4(sc->hda,SD7);sample.status=bus_read_1(sc->hda,SD7+3);
    sample.dpib=er4(sc->hda,ESSX_DPIB7);sample.lpib=er4(sc->hda,SD7+4);
    result=essx_check_sample(&sample,position);
    if(result!=ESSX_SAMPLE_OK) {
        device_printf(sc->dev,"PCM DMA fault=%u SDCTL=0x%06x SDSTS=0x%02x DPIB=%u LPIB=%u\n",
            result,sample.ctl&0xffffffu,sample.status,sample.dpib,sample.lpib);
        return(EIO);
    }
    if(er4(sc->dsp,HIPCTDR)&ESSX_DSP_BUSY) {
        if(notify_ack(sc,&notification))return(EIO);
        device_printf(sc->dev,"PCM firmware notification=0x%08x; playback stopped\n",notification);
        return(EIO);
    }
    return(0);
}

/* Called with the sleepable control lock. PCM callbacks never enter here. */
static int
pcm_hardware_stop(struct essx_softc *sc)
{
    int error=0,e;
    e=essx_codec_restore();if(e)error=e;
    if(sc->streaming) {
        e=essx_stream_command(sc,SOF_IPC_STREAM_TRIG_STOP,"STREAM_STOP");
        if(e&&!error)error=e;
        sc->streaming=false;
    }
    if(sc->saved || sc->powered) {
        e=quiesce(sc);if(e&&!error)error=e;
    }
    sc->pcm_enabled=false;
    if(sc->firmware)firmware_put(sc->firmware,0);
    sc->firmware=NULL;bzero(&sc->image,sizeof(sc->image));
    return(error);
}
static int
pcm_hardware_start(struct essx_softc *sc)
{
    struct sof_ipc_pm_ctx_probe {struct sof_ipc_cmd_hdr h;uint32_t words[17];} query;
    uint32_t first,last;
    int error,cleanup;
    if(sc->poisoned)return(EBUSY);
    if(sc->pcm_child==NULL || !device_is_attached(sc->pcm_child))return(ENXIO);
    sc->phase=0;
    sc->stage="codec-preflight";error=essx_codec_available();if(error)return(error);
    sc->stage="firmware-image";
    sc->firmware=firmware_get_flags("essx8336_sof",FIRMWARE_GET_NOWARN);
    if(!sc->firmware)return(ENOENT);
    error=essx_parse_image(sc->firmware->data,sc->firmware->datasize,&sc->image);if(error)goto failed;
    bzero(sc->dma.vaddr,ESSX_MEM_SIZE);
    memcpy((uint8_t *)sc->dma.vaddr+ESSX_FW_DATA,sc->image.payload,sc->image.payload_size);
    error=boot_firmware(sc);if(error)goto failed;
    bzero(&query,sizeof(query));query.h.size=sizeof(query);query.h.cmd=SOF_IPC_GLB_PM_MSG|SOF_IPC_PM_CTX_SIZE;
    query.words[0]=28;
    error=essx_ipc(sc,&query,sizeof(query),"PM_CTX_SIZE");if(error)goto failed;
    error=setup_stream(sc,SD7,ESSX_AUDIO_DATA,ESSX_AUDIO_SIZE,ESSX_OUT_BDL,0x31,true);
    if(error)goto failed;
    error=essx_make_topology(sc);if(error)goto failed;
    sc->stage="codec-prepare";error=essx_codec_prepare(sc->dac_attenuation);if(error)goto failed;
    bus_dmamap_sync(sc->dma.tag,sc->dma.map,BUS_DMASYNC_PREWRITE);
    ew4(sc->hda,SD7,(er4(sc->hda,SD7)&0xffffffu)|RUN);
    error=expect_bits(sc->hda,SD7,RUN|RESET,RUN,50000);if(error)goto failed;
    sc->streaming=true;
    error=essx_stream_command(sc,SOF_IPC_STREAM_TRIG_START,"STREAM_START");if(error)goto failed;
    sc->stage="silent-DMA-check";error=essx_pcm_position(sc,&first);if(error)goto failed;
    pause("essxverify",MAX(1,hz/50));
    error=essx_pcm_position(sc,&last);if(error)goto failed;
    if(last==first) {error=EIO;goto failed;}
    sc->stage="codec-output";
    error=essx_codec_route(sc->amp_gpio,sc->dac_attenuation,sc->output_mode);
    if(error)goto failed;
    sc->phase=4;sc->pcm_enabled=true;
    error=essx_pcm_arm(sc->pcm_child,true);if(error)goto failed;
    device_printf(sc->dev,"PCM enabled: 48k stereo GPIO174=%d DMA=%u->%u; idle ring is silence\n",
        sc->amp_gpio,first,last);
    return(0);
failed:
    device_printf(sc->dev,"PCM enable failed stage=%s errno=%d\n",sc->stage,error);
    cleanup=pcm_hardware_stop(sc);
    if(cleanup)device_printf(sc->dev,"PCM cleanup errno=%d\n",cleanup);
    return(error);
}
static int
pcm_enable_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value,error;
    sx_xlock(&sc->lock);value=sc->pcm_enabled;sx_xunlock(&sc->lock);
    error=sysctl_handle_int(oidp,&value,0,req);
    if(error || !req->newptr)return(error);
    if(value!=0 && value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching)error=ENXIO;
    else if(value && !sc->pcm_enabled)error=pcm_hardware_start(sc);
    else if(!value && sc->pcm_enabled) {
        error=device_is_attached(sc->pcm_child)?essx_pcm_arm(sc->pcm_child,false):0;
        if(!error)error=pcm_hardware_stop(sc);
    }
    sc->last_error=error;
    sx_xunlock(&sc->lock);return(error);
}
static int
amp_gpio_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value,error;
    sx_xlock(&sc->lock);value=sc->amp_gpio;sx_xunlock(&sc->lock);
    error=sysctl_handle_int(oidp,&value,0,req);
    if(error || !req->newptr)return(error);
    if(value!=0 && value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching)error=ENXIO;
    else if(sc->pcm_enabled)error=EBUSY;
    else sc->amp_gpio=value;
    sx_xunlock(&sc->lock);return(error);
}
static int
attenuation_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value,error;
    sx_xlock(&sc->lock);value=sc->dac_attenuation;sx_xunlock(&sc->lock);
    error=sysctl_handle_int(oidp,&value,0,req);
    if(error || !req->newptr)return(error);
    if(value<0 || value>48)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching)error=ENXIO;
    else if(sc->pcm_enabled)error=EBUSY;
    else sc->dac_attenuation=value;
    sx_xunlock(&sc->lock);return(error);
}
static int
output_mode_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value,error;
    sx_xlock(&sc->lock);value=sc->output_mode;sx_xunlock(&sc->lock);
    error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return(error);
    if(value<0||value>2)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching)error=ENXIO;
    else if(!sc->pcm_enabled||sc->poisoned)error=EBUSY;
    else {
        error=essx_codec_route(sc->amp_gpio,sc->dac_attenuation,value);
        if(!error)sc->output_mode=value;
    }
    sc->last_error=error;
    sx_xunlock(&sc->lock);return(error);
}

static int
jack_status_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value=0,error;
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled)error=ENXIO;
    else error=arg2?essx_codec_jack_pin(&value):essx_codec_jack_flags(&value);
    sx_xunlock(&sc->lock);
    if(error)return(error);
    return(sysctl_handle_int(oidp,&value,0,req));
}

static int
jack_poll_prepare_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value=0,error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return(error);
    if(value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled||sc->poisoned)error=EBUSY;
    else error=essx_codec_jack_poll_prepare();
    sx_xunlock(&sc->lock);return(error);
}

static int
codec_snapshot_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    char text[256];
    int error;
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled)error=ENXIO;
    else error=essx_codec_snapshot(text,sizeof(text));
    sx_xunlock(&sc->lock);
    if(error)return(error);
    return(sysctl_handle_string(oidp,text,sizeof(text),req));
}

static int
jack_gpio_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    char text[256];
    int error;
    sx_xlock(&sc->lock);
    error=sc->detaching?ENXIO:essx_jack_gpio_snapshot(text,sizeof(text));
    sx_xunlock(&sc->lock);
    if(error)return(error);
    return(sysctl_handle_string(oidp,text,sizeof(text),req));
}

static int
jack_detect_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value=essx_codec_jack_detect_state();
    int error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return(error);
    if(value!=0&&value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled||sc->poisoned)error=EBUSY;
    else error=essx_codec_jack_detect(value);
    sx_xunlock(&sc->lock);return(error);
}

static int
micbias_trial_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value=essx_codec_micbias_state();
    int error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return(error);
    if(value!=0&&value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled||sc->poisoned)error=EBUSY;
    else error=essx_codec_micbias_trial(value);
    sx_xunlock(&sc->lock);return(error);
}

static int
bias_playback_sysctl(SYSCTL_HANDLER_ARGS)
{
    struct essx_softc *sc=arg1;
    int value=essx_codec_bias_playback_state();
    int error=sysctl_handle_int(oidp,&value,0,req);
    if(error||!req->newptr)return(error);
    if(value!=0&&value!=1)return(EINVAL);
    error=priv_check(req->td,PRIV_DRIVER);if(error)return(error);
    sx_xlock(&sc->lock);
    if(sc->detaching||!sc->pcm_enabled||sc->poisoned)error=EBUSY;
    else error=essx_codec_bias_playback(value);
    sx_xunlock(&sc->lock);return(error);
}

static int
essx_probe(device_t dev)
{
    if(pci_get_devid(dev)!=0xa0c88086 || pci_get_domain(dev)!=0 ||
        pci_get_bus(dev)!=0 || pci_get_slot(dev)!=31 || pci_get_function(dev)!=3 ||
        pci_get_subvendor(dev)!=0x1e50 || pci_get_subdevice(dev)!=0x6004)
        return(ENXIO);
    device_set_desc(dev,"Casper ES8336 SOF playback 0.20.0-rc1 (experimental)");
    return(BUS_PROBE_SPECIFIC);
}
static int
essx_attach(device_t dev)
{
    struct essx_softc *sc=device_get_softc(dev);
    struct sysctl_oid *tree=device_get_sysctl_tree(dev);
    struct sysctl_ctx_list *ctx=device_get_sysctl_ctx(dev);
    int r0=PCIR_BAR(0),r4=PCIR_BAR(4);
    sc->dev=dev;sx_init(&sc->lock,"essxsof control");
    sc->dac_attenuation=12; /* Known working baseline for manual loading. */
    strlcpy(sc->driver_version,"0.20.0-rc1",sizeof(sc->driver_version));
    sc->hda=bus_alloc_resource_any(dev,SYS_RES_MEMORY,&r0,RF_ACTIVE);
    sc->dsp=bus_alloc_resource_any(dev,SYS_RES_MEMORY,&r4,RF_ACTIVE);
    if(!sc->hda||!sc->dsp)goto failed;
    if(rman_get_size(sc->hda)!=0x4000||rman_get_size(sc->dsp)!=0x100000 ||
        bus_read_2(sc->hda,0)!=0x9701 || (er4(sc->hda,0x800)&0x0fff0000)!=0x30000 ||
        (er4(sc->hda,0x700)&0x0fff0000)!=0x40000)goto failed;
    if(dma_alloc(sc))goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"pcm_enable",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,pcm_enable_sysctl,"I",
        "Root enables or disables persistent SOF PCM playback")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"amp_gpio",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,amp_gpio_sysctl,"I",
        "GPIO174 output level, selectable only while PCM disabled")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"dac_attenuation",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,attenuation_sysctl,"I",
        "DAC attenuation in dB (0..48), change only while PCM disabled")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"output_mode",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,output_mode_sysctl,"I",
        "Root output: 0=speakers, 1=headphone test amp off, 2=headphone test amp on")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_poll_prepare",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,jack_poll_prepare_sysctl,"I",
        "Root writes 1 to select HP detection input with codec IRQ disabled")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"codec_snapshot",
        CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_MPSAFE,sc,0,codec_snapshot_sysctl,"A",
        "Read-only playback/detection register snapshot (sequential reads)")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_gpio",
        CTLTYPE_STRING|CTLFLAG_RD|CTLFLAG_MPSAFE,sc,0,jack_gpio_sysctl,"A",
        "GPIO IRQ resource, event registers and interrupt counter")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_detect_enable",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,jack_detect_sysctl,"I",
        "Root enables codec jack IRQ after host handler ownership")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_micbias_trial",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,micbias_trial_sysctl,"I",
        "Root temporarily enables existing mic-bias supply for jack status comparison")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_bias_playback",
        CTLTYPE_INT|CTLFLAG_RW|CTLFLAG_MPSAFE,sc,0,bias_playback_sysctl,"I",
        "Root permits attenuated playback/route changes during held-bias test")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_flags",
        CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_MPSAFE,sc,0,jack_status_sysctl,"I",
        "Raw codec 0x4f; jack presence/polarity not yet calibrated")==NULL)goto failed;
    if(SYSCTL_ADD_PROC(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"jack_pin",
        CTLTYPE_INT|CTLFLAG_RD|CTLFLAG_MPSAFE,sc,1,jack_status_sysctl,"I",
        "GPIO173 sampled input; no pin writes")==NULL)goto failed;
    SYSCTL_ADD_STRING(ctx,SYSCTL_CHILDREN(tree),OID_AUTO,"driver_version",
        CTLFLAG_RD,sc->driver_version,sizeof(sc->driver_version),"Driver release");
    sc->pcm_child=device_add_child(dev,"pcm",-1);
    if(sc->pcm_child==NULL)goto failed;
    bus_attach_children(dev);
    device_printf(dev,"exclusive PCI ownership; PCM idle until root requests pcm_enable=1\n");
    return(0);
failed:
    if(sc->pcm_child) {device_delete_child(dev,sc->pcm_child);sc->pcm_child=NULL;}
    dma_free(sc);
    if(sc->dsp)bus_release_resource(dev,SYS_RES_MEMORY,r4,sc->dsp);
    if(sc->hda)bus_release_resource(dev,SYS_RES_MEMORY,r0,sc->hda);
    sc->dsp=sc->hda=NULL;sx_destroy(&sc->lock);return(ENXIO);
}
static int
essx_detach(device_t dev)
{
    struct essx_softc *sc=device_get_softc(dev);
    int error;
    sx_xlock(&sc->lock);
    if(sc->poisoned) {sx_xunlock(&sc->lock);return(EBUSY);}
    sc->detaching=true;
    /* Child callouts and open PCM channels must drain before DMA/BARs. */
    if(sc->pcm_child) {
        error=device_delete_child(dev,sc->pcm_child);
        if(error) {sc->detaching=false;sx_xunlock(&sc->lock);return(error);}
        sc->pcm_child=NULL;
    }
    error=pcm_hardware_stop(sc);
    if(error || sc->poisoned) {sc->detaching=false;sx_xunlock(&sc->lock);return(error?error:EBUSY);}
    dma_free(sc);sx_xunlock(&sc->lock);
    error=sysctl_ctx_free(device_get_sysctl_ctx(dev));
    if(error) {sx_xlock(&sc->lock);sc->detaching=false;sx_xunlock(&sc->lock);return(error);}
    bus_release_resource(dev,SYS_RES_MEMORY,PCIR_BAR(4),sc->dsp);
    bus_release_resource(dev,SYS_RES_MEMORY,PCIR_BAR(0),sc->hda);
    sx_destroy(&sc->lock);return(0);
}
static int essx_suspend(device_t dev) {(void)dev;return(EBUSY);}
static int essx_shutdown(device_t dev) {
    struct essx_softc *sc=device_get_softc(dev);
    sx_xlock(&sc->lock);
    /* The system shutdown sequence shuts PCM children down first. A busy
     * stream is stopped by the child shutdown callback before resources. */
    if(sc->pcm_child)(void)essx_pcm_halt(sc->pcm_child);
    (void)pcm_hardware_stop(sc);
    sx_xunlock(&sc->lock);return(0);
}
static device_method_t essx_methods[]={
    DEVMETHOD(device_probe,essx_probe),DEVMETHOD(device_attach,essx_attach),
    DEVMETHOD(device_detach,essx_detach),DEVMETHOD(device_suspend,essx_suspend),
    DEVMETHOD(device_shutdown,essx_shutdown),DEVMETHOD_END};
static driver_t essx_driver={.name="essxsof",.methods=essx_methods,.size=sizeof(struct essx_softc)};
DRIVER_MODULE(essxsof,pci,essx_driver,NULL,NULL);
MODULE_DEPEND(essxsof,pci,1,1,1);
MODULE_DEPEND(essxsof,acpi,1,1,1);
MODULE_DEPEND(essxsof,iicbus,1,1,1);
MODULE_DEPEND(essxsof,firmware,1,1,1);
MODULE_VERSION(essxsof,18);
