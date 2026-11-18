/*-
 * SPDX-License-Identifier: BSD-2-Clause
 * Fixed-rate FreeBSD playback child for the ES8336 SOF bring-up driver.
 * No sleeping IPC/I2C work is performed in PCM callbacks or callouts.
 * DSP/SSP run a silent cyclic ring while enabled; the PCM channel maps
 * its logical start to the current hardware position on each START.
 */
#ifdef ESSX_PCM_TEST
#include "tests/pcm_fake.h"
#else
#include <dev/sound/pcm/sound.h>
#include <sys/callout.h>
#include <dev/sound/sndstat.h>
#include "mixer_if.h"
#include "essxsof.h"
#include "essx_ring.h"
#endif

struct essx_pcm {
    struct snddev_info info; /* sound(4) requires this at offset zero */
    device_t dev;
    struct essx_softc *parent;
    struct pcm_channel *channel;
    struct snd_dbuf *buffer;
    struct mtx lock;
    struct callout poll;
    uint8_t *front;
    uint32_t base,position,notified,epoch;
    bool armed,running,dying,wanted;
    unsigned polls,starts,start_requests,deferred;
    int fault;
};
CTASSERT(offsetof(struct essx_pcm,info)==0);
static uint32_t essx_formats[]={SND_FORMAT(AFMT_S32_LE,2,0),0};
static struct pcmchan_caps essx_caps={48000,48000,essx_formats,0};
static void essx_poll(void *);

static uint8_t *
audio_ring(struct essx_pcm *p)
{
    return((uint8_t *)p->parent->dma.vaddr+ESSX_AUDIO_DATA);
}
static void
silence_ring(struct essx_pcm *p)
{
    bzero(audio_ring(p),ESSX_AUDIO_SIZE);
    bus_dmamap_sync(p->parent->dma.tag,p->parent->dma.map,BUS_DMASYNC_PREWRITE);
}
static void
copy_ring(struct essx_pcm *p)
{
    CHN_LOCKASSERT(p->channel);
    mtx_assert(&p->lock,MA_OWNED);
    (void)essx_ring_copy(audio_ring(p),p->front,p->base);
    bus_dmamap_sync(p->parent->dma.tag,p->parent->dma.map,BUS_DMASYNC_PREWRITE);
}

/* The sound core marks a channel triggered before calling the driver;
 * it does not unwind all that state when START returns an error. Accept
 * early application STARTs without touching hardware, then fulfill them
 * when the root-controlled firmware/codec startup arms this channel. */
static int
start_locked(struct essx_pcm *p)
{
    uint32_t pos;
    int error;
    CHN_LOCKASSERT(p->channel);mtx_assert(&p->lock,MA_OWNED);
    if(p->running)return(0);
    error=essx_pcm_position(p->parent,&pos);if(error)return(error);
    p->base=pos&~7u;p->position=p->notified=0;
    p->epoch++;p->running=true;p->starts++;
    copy_ring(p);
    callout_reset(&p->poll,MAX(1,hz/200),essx_poll,p);
    device_printf(p->dev,"PCM START accepted: base=%u starts=%u\n",p->base,p->starts);
    return(0);
}

static void
essx_poll(void *arg)
{
    struct essx_pcm *p=arg;
    struct pcm_channel *ch=p->channel;
    uint32_t pos,delta,epoch=p->epoch;
    bool notify;
    int error;
    mtx_assert(&p->lock,MA_OWNED);
    if(p->dying || !p->armed || !p->running)return;
    error=essx_pcm_position(p->parent,&pos);
    if(error) {
        p->fault=error;p->running=false;silence_ring(p);
        device_printf(p->dev,"playback fault errno=%d; close player and send report\n",error);
        return;
    }
    p->polls++;
    p->position=essx_ring_pos(pos,p->base);
    delta=(p->position+ESSX_AUDIO_SIZE-p->notified)%ESSX_AUDIO_SIZE;
    notify=delta>=4096;
    if(notify)p->notified=p->position;
    mtx_unlock(&p->lock);
    if(notify)chn_intr(ch);
    /* Lock order is always channel -> driver. Detach drains this callout
     * before destroying the channel; STOP never waits with CHN_LOCK held. */
    CHN_LOCK(ch);
    mtx_lock(&p->lock);
    if(!p->dying && p->armed && p->running && p->epoch==epoch)copy_ring(p);
    mtx_unlock(&p->lock);
    CHN_UNLOCK(ch);
    mtx_lock(&p->lock);
    if(!p->dying && p->armed && p->running && p->epoch==epoch)
        callout_schedule(&p->poll,MAX(1,hz/200));
}

static void *
essx_chan_init(kobj_t obj,void *data,struct snd_dbuf *b,
    struct pcm_channel *c,int dir)
{
    struct essx_pcm *p=data;
    if(dir!=PCMDIR_PLAY || p->channel!=NULL)return(NULL);
    p->front=malloc(ESSX_AUDIO_SIZE,M_DEVBUF,M_WAITOK|M_ZERO);
    p->channel=c;p->buffer=b;
    if(sndbuf_setup(b,p->front,ESSX_AUDIO_SIZE)!=0 || sndbuf_resize(b,12,4096)!=0) {
        free(p->front,M_DEVBUF);p->front=NULL;b->buf=NULL;
        p->channel=NULL;p->buffer=NULL;return(NULL);
    }
    return(p);
}
static int
essx_chan_free(kobj_t obj,void *data)
{
    struct essx_pcm *p=data;
    free(p->front,M_DEVBUF);p->front=NULL;
    p->buffer->buf=NULL;p->buffer=NULL;p->channel=NULL;
    return(0);
}
static int
essx_chan_format(kobj_t obj,void *data,uint32_t format)
{
    return(format==essx_formats[0]?0:EINVAL);
}
static uint32_t essx_chan_speed(kobj_t obj,void *data,uint32_t speed) {return(48000);}
static uint32_t essx_chan_block(kobj_t obj,void *data,uint32_t size) {return(4096);}
static int
essx_chan_trigger(kobj_t obj,void *data,int go)
{
    struct essx_pcm *p=data;
    int error=0;
    mtx_lock(&p->lock);
    switch(go) {
    case PCMTRIG_START:
        p->start_requests++;
        if(p->dying) {error=ENXIO;break;}
        if(p->fault) {error=p->fault;break;}
        p->wanted=true;
        if(!p->armed) {
            p->deferred++;
            device_printf(p->dev,"PCM START deferred until root enable (request=%u)\n",p->start_requests);
            break;
        }
        error=start_locked(p);
        break;
    case PCMTRIG_STOP:
    case PCMTRIG_ABORT:
        p->wanted=false;p->running=false;p->epoch++;callout_stop(&p->poll);
        silence_ring(p);
        break;
    default:break;
    }
    mtx_unlock(&p->lock);return(error);
}
static uint32_t
essx_chan_ptr(kobj_t obj,void *data)
{
    struct essx_pcm *p=data;
    uint32_t pos;
    mtx_lock(&p->lock);pos=p->position;mtx_unlock(&p->lock);
    return(pos);
}
static struct pcmchan_caps *essx_chan_caps(kobj_t obj,void *data) {return(&essx_caps);}
#ifndef ESSX_PCM_TEST
static kobj_method_t essx_chan_methods[]={
    KOBJMETHOD(channel_init,essx_chan_init),KOBJMETHOD(channel_free,essx_chan_free),
    KOBJMETHOD(channel_setformat,essx_chan_format),KOBJMETHOD(channel_setspeed,essx_chan_speed),
    KOBJMETHOD(channel_setblocksize,essx_chan_block),KOBJMETHOD(channel_trigger,essx_chan_trigger),
    KOBJMETHOD(channel_getptr,essx_chan_ptr),KOBJMETHOD(channel_getcaps,essx_chan_caps),KOBJMETHOD_END};
CHANNEL_DECLARE(essx_chan);

static int
essx_mixer_init(struct snd_mixer *m)
{
    struct essx_pcm *p=mix_getdevinfo(m);
    pcm_setflags(p->dev,pcm_getflags(p->dev)|SD_F_SOFTPCMVOL);
    mix_setdevs(m,SOUND_MASK_PCM|SOUND_MASK_VOLUME);mix_setrecdevs(m,0);
    mix_setparentchild(m,SOUND_MIXER_VOLUME,SOUND_MASK_PCM);
    return(0);
}
static int
essx_mixer_set(struct snd_mixer *m,unsigned dev,unsigned left,unsigned right)
{
    return(MIN(left,100)|(MIN(right,100)<<8));
}
static kobj_method_t essx_mixer_methods[]={
    KOBJMETHOD(mixer_init,essx_mixer_init),KOBJMETHOD(mixer_set,essx_mixer_set),KOBJMETHOD_END};
MIXER_DECLARE(essx_mixer);
#endif

int
essx_pcm_arm(device_t dev,bool enable)
{
    struct essx_pcm *p;
    int error=0;
    if(dev==NULL || !device_is_attached(dev))return(ENXIO);
    p=device_get_softc(dev);
    if(p->channel==NULL)return(ENXIO);
    CHN_LOCK(p->channel);
    mtx_lock(&p->lock);
    if(p->dying || (!enable && p->running)) {
        error=EBUSY;goto out;
    }
    p->armed=enable;p->fault=0;
    if(!enable) {p->epoch++;callout_stop(&p->poll);silence_ring(p);}
    else if(p->wanted) {
        error=start_locked(p);
        if(error) {p->armed=false;p->fault=error;}
    }
out:
    mtx_unlock(&p->lock);
    CHN_UNLOCK(p->channel);
    if(!enable&&!error)callout_drain(&p->poll);
    return(error);
}
int
essx_pcm_halt(device_t dev)
{
    struct essx_pcm *p;
    if(dev==NULL || !device_is_attached(dev))return(0);
    p=device_get_softc(dev);
    mtx_lock(&p->lock);p->dying=true;p->armed=false;p->wanted=false;p->running=false;p->epoch++;
    callout_stop(&p->poll);silence_ring(p);mtx_unlock(&p->lock);
    callout_drain(&p->poll);return(0);
}
#ifndef ESSX_PCM_TEST
static int
essx_pcm_probe(device_t dev)
{
    device_set_desc(dev,"Casper ES8336 SOF speaker playback 0.20.0-rc1");
    return(BUS_PROBE_DEFAULT);
}

/* release/15.1 pcm_unregister() assumes a successful dsp_make_dev().
 * On an attach failure before a cdev exists, dispose the still-inactive
 * channel and initialized core state without calling destroy_dev(NULL).
 * No application can have opened this device on this path. */
static void
partial_pcm_cleanup(struct essx_pcm *p)
{
    struct snddev_info *d=&p->info;
    if(d->dsp_dev!=NULL) {(void)pcm_unregister(p->dev);return;}
    if(PCM_REGISTERED(d)) {
        d->flags&=~SD_F_REGISTERED;
        if(d->play_sysctl_tree)sysctl_ctx_free(&d->play_sysctl_ctx);
        if(d->rec_sysctl_tree)sysctl_ctx_free(&d->rec_sysctl_ctx);
        sndstat_unregister(p->dev);
        if(snd_unit==device_get_unit(p->dev))snd_unit=-1;
    }
    (void)mixer_uninit(p->dev);
    PCM_LOCK(d);PCM_ACQUIRE(d);PCM_UNLOCK(d);
    while(!CHN_EMPTY(d,channels.pcm))chn_kill(CHN_FIRST(d,channels.pcm));
    PCM_RELEASE_QUICK(d);
    if(d->p_unr)delete_unrhdr(d->p_unr);
    if(d->vp_unr)delete_unrhdr(d->vp_unr);
    if(d->r_unr)delete_unrhdr(d->r_unr);
    if(d->vr_unr)delete_unrhdr(d->vr_unr);
    cv_destroy(&d->cv);mtx_destroy(&d->lock);
}
static int
essx_pcm_attach(device_t dev)
{
    struct essx_pcm *p=device_get_softc(dev);
    char status[SND_STATUSLEN];
    int error;
    p->dev=dev;p->parent=device_get_softc(device_get_parent(dev));
    mtx_init(&p->lock,"essx pcm","essx pcm",MTX_DEF);
    callout_init_mtx(&p->poll,&p->lock,0);
    pcm_setflags(dev,pcm_getflags(dev)|SD_F_MPSAFE|SD_F_SOFTPCMVOL);
    pcm_getbuffersize(dev,ESSX_AUDIO_SIZE,ESSX_AUDIO_SIZE,ESSX_AUDIO_SIZE);
    pcm_init(dev,p);
    error=pcm_addchan(dev,PCMDIR_PLAY,&essx_chan_class,p);
    if(error)goto failed;
    snprintf(status,sizeof(status),"SOF IPC3 SSP0 / ES8336 on %s",
        device_get_nameunit(device_get_parent(dev)));
    error=pcm_register(dev,status);if(error)goto failed;
    error=mixer_init(dev,&essx_mixer_class,p);if(error)goto failed;
    SYSCTL_ADD_UINT(device_get_sysctl_ctx(dev),SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO,"essx_starts",CTLFLAG_RD,&p->starts,0,"PCM starts");
    SYSCTL_ADD_UINT(device_get_sysctl_ctx(dev),SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO,"essx_start_requests",CTLFLAG_RD,&p->start_requests,0,"PCM START callbacks");
    SYSCTL_ADD_UINT(device_get_sysctl_ctx(dev),SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO,"essx_deferred",CTLFLAG_RD,&p->deferred,0,"STARTs accepted before root enable");
    SYSCTL_ADD_UINT(device_get_sysctl_ctx(dev),SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO,"essx_polls",CTLFLAG_RD,&p->polls,0,"Successful DMA position polls");
    SYSCTL_ADD_INT(device_get_sysctl_ctx(dev),SYSCTL_CHILDREN(device_get_sysctl_tree(dev)),
        OID_AUTO,"essx_fault",CTLFLAG_RD,&p->fault,0,"Playback fault errno");
    device_printf(dev,"/dev/dsp%d registered; inactive until root enables pcm_enable=1\n",
        device_get_unit(dev));
    return(0);
failed:
    partial_pcm_cleanup(p);
    mtx_destroy(&p->lock);return(error);
}
static int
essx_pcm_detach(device_t dev)
{
    struct essx_pcm *p=device_get_softc(dev);
    int error;
    (void)essx_pcm_halt(dev);
    error=pcm_unregister(dev);
    if(!error)mtx_destroy(&p->lock);
    return(error);
}
static device_method_t essx_pcm_methods[]={
    DEVMETHOD(device_probe,essx_pcm_probe),DEVMETHOD(device_attach,essx_pcm_attach),
    DEVMETHOD(device_detach,essx_pcm_detach),DEVMETHOD_END};
static driver_t essx_pcm_driver={.name="pcm",.methods=essx_pcm_methods,.size=sizeof(struct essx_pcm)};
DRIVER_MODULE(essxpcm,essxsof,essx_pcm_driver,NULL,NULL);
MODULE_DEPEND(essxsof,sound,SOUND_MINVER,SOUND_PREFVER,SOUND_MAXVER);

#endif
