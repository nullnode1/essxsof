/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * Experimental ES8336 playback control for the Casper NB F500.
 * Register fields and playback routing follow Linux v6.12 es8316.c/es8316.h
 * (Copyright Everest Semiconductor Co.,Ltd) and sof_es8336.c.
 * Independent FreeBSD I2C/ACPI glue; no ALSA or LinuxKPI dependency.
 */
#ifdef ESSX_CODEC_TEST
#include "tests/codec_fake.h"
#else
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/systm.h>
#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>
#include <dev/iicbus/iic.h>
#include <dev/iicbus/iicbus.h>
#include <dev/iicbus/iiconf.h>
#include "bus_if.h"
#include "device_if.h"
#include "essxsof.h"
#endif

#define AMP_PAD 0x0908000eU
#define JACK_IRQ_PAD 0x0908000dU
#define JACK_FLAG_REG 0x4f
static const uint8_t codec_regs[]={0x30,0x00,0x0c,0x03,0x01,0x09,0x0b,
    0x06,0x07,0x33,0x34,0x0d,0x14,0x15,0x18,0x19,0x1a,0x17,0x2f,
    0x4d,0x4e}; /* input selection / IRQ control: journal before polling setup */
struct essx_codec {
    device_t dev;
    uint8_t saved[nitems(codec_regs)];
    bool touched[nitems(codec_regs)], active, jack_detect_enabled, micbias_trial, bias_playback;
    uint8_t trial_bias_original,trial_mute_original;
    uint32_t pad_original;
};
static struct essx_codec *codec;
static struct sx codec_lock;
SX_SYSINIT(essx_codec_lock,&codec_lock,"essx codec");

static int
codec_read(struct essx_codec *c,uint8_t reg,uint8_t *val)
{
    struct iic_msg msgs[2]={
        {.slave=0x22,.flags=IIC_M_WR|IIC_M_NOSTOP,.len=1,.buf=&reg},
        {.slave=0x22,.flags=IIC_M_RD,.len=1,.buf=val}};
    return(iic2errno(iicbus_transfer_excl(c->dev,msgs,2,IIC_WAIT)));
}

static int
codec_write(struct essx_codec *c,uint8_t reg,uint8_t val)
{
    uint8_t data[2]={reg,val},readback=0;
    struct iic_msg msg={.slave=0x22,.flags=IIC_M_WR,.len=2,.buf=data};
    int error=iic2errno(iicbus_transfer_excl(c->dev,&msg,1,IIC_WAIT));
    if (!error) error=codec_read(c,reg,&readback);
    if (!error && readback!=val) error=EIO;
    if (error) device_printf(c->dev,"register 0x%02x write/readback errno=%d\n",reg,error);
    return(error);
}

static int
codec_update(struct essx_codec *c,uint8_t reg,uint8_t mask,uint8_t value)
{
    uint8_t old;
    unsigned i;
    int error;
    for(i=0;i<nitems(codec_regs);i++) if(codec_regs[i]==reg) break;
    if(i==nitems(codec_regs)) return(EINVAL);
    error=codec_read(c,reg,&old);
    if(error) return(error);
    /* Include failed writes in rollback: an I2C error can follow a partial write. */
    c->touched[i]=true;
    return(codec_write(c,reg,(old&~mask)|(value&mask)));
}

static int
pad_get_for(uint32_t pad,const char *path,uint32_t *v)
{
    ACPI_OBJECT in={.Integer={.Type=ACPI_TYPE_INTEGER,.Value=pad}},out={0};
    ACPI_OBJECT_LIST args={.Count=1,.Pointer=&in};
    ACPI_BUFFER buf={.Length=sizeof(out),.Pointer=&out};
    ACPI_STATUS s=AcpiEvaluateObject(NULL,__DECONST(char *,path),&args,&buf);
    if(ACPI_FAILURE(s)||out.Type!=ACPI_TYPE_INTEGER||out.Integer.Value>UINT32_MAX)
        return(EIO);
    *v=(uint32_t)out.Integer.Value;
    return(0);
}

static int
pad_get(const char *path,uint32_t *v)
{
    return(pad_get_for(AMP_PAD,path,v));
}

static int
pad_set(uint32_t level)
{
    ACPI_OBJECT in[2];
    ACPI_OBJECT_LIST args={.Count=2,.Pointer=in};
    uint32_t before,after;
    int error;
    if(level>1) return(EINVAL);
    error=pad_get("\\_SB.GPC0",&before);
    if(error || before==UINT32_MAX || (before&(7u<<10|1u<<8))!=0 ||
        (before&(1u<<9))==0) return(ENXIO);
    bzero(in,sizeof(in));
    in[0].Integer.Type=in[1].Integer.Type=ACPI_TYPE_INTEGER;
    in[0].Integer.Value=AMP_PAD; in[1].Integer.Value=level;
    if(ACPI_FAILURE(AcpiEvaluateObject(NULL,"\\_SB.SGOV",&args,NULL))) return(EIO);
    error=pad_get("\\_SB.GPC0",&after);
    if(error || after==UINT32_MAX || (after&1)!=level || ((after^before)&~3u)) return(EIO);
    return(0);
}

static int
codec_guard(struct essx_codec *c)
{
    UINT32 pchs;
    uint32_t n,dw,host,owner;
    if(ACPI_FAILURE(acpi_GetInteger(NULL,"\\PCHS",&pchs))||pchs!=2) return(ENXIO);
    if(pad_get("\\_SB.GNUM",&n)||n!=174||pad_get("\\_SB.GPC0",&dw)||
        dw==UINT32_MAX || (dw&(7u<<10|1u<<8)) || !(dw&(1u<<9)) ||
        pad_get("\\_SB.GHPO",&host)||host!=1||pad_get("\\_SB.GGPO",&owner)||owner!=0)
        return(ENXIO);
    c->pad_original=dw&1;
    return(0);
}

int
essx_codec_available(void)
{
    int error;
    sx_xlock(&codec_lock);
    error=codec==NULL?ENXIO:(codec->active?EBUSY:codec_guard(codec));
    sx_xunlock(&codec_lock);
    return(error);
}

static int
codec_rollback(struct essx_codec *c)
{
    int error=0,e;
    unsigned i;
    if(!c->active) return(0);
    /* Mask the host source even after an I2C error; keep busy on error. */
    if(c->jack_detect_enabled) {
        e=codec_update(c,0x4e,0x02,0);if(e)error=e;
        e=essx_jack_gpio_disarm();if(e)error=e;
        if(!error)c->jack_detect_enabled=false;
    }
    /* Best effort on every element even if an earlier write fails. */
    e=codec_update(c,0x30,0x20,0x20); if(e)error=e;
    e=pad_set(c->pad_original); if(e)error=e;
    pause("essxmute",MAX(1,hz/50));
    for(i=nitems(codec_regs);i>0;i--) if(c->touched[i-1]) {
        e=codec_write(c,codec_regs[i-1],c->saved[i-1]); if(e)error=e;
    }
    if(!error) {c->active=false;c->micbias_trial=false;c->bias_playback=false;device_unbusy(c->dev);}
    device_printf(c->dev,"codec/GPIO restore=%s errno=%d\n",error?"failed":"ok",error);
    return(error);
}

int
essx_codec_prepare(unsigned attenuation_db)
{
    struct essx_codec *c;
    unsigned i;
    int error;
    if(attenuation_db>48)return(EINVAL);
    sx_xlock(&codec_lock); c=codec;
    if(c==NULL) {error=ENXIO;goto out;}
    if(c->active) {error=EBUSY;goto out;}
    error=codec_guard(c); if(error)goto out;
    for(i=0;i<nitems(codec_regs);i++) {
        error=codec_read(c,codec_regs[i],&c->saved[i]); if(error)goto out;
        c->touched[i]=false;
    }
    c->active=true;device_busy(c->dev);
#define U(r,m,v) do {error=codec_update(c,(r),(m),(v));if(error)goto failed;}while(0)
    U(0x30,0x20,0x20);              /* DAC mute before every other change */
    U(0x00,0xff,0x80);              /* CSM on; no global 0x3f reset */
    U(0x0c,0xff,0xff); U(0x03,0xff,0x32);
    U(0x01,0xf4,0x74);              /* MCLK/BCLK inputs, DAC+charge-pump clock */
    U(0x09,0xbf,4); U(0x0b,0x3f,0); /* consumer, I2S, 24-bit */
    U(0x06,0x0f,1); U(0x07,0xff,0x90); /* 19.2 MHz / 48 kHz = 400 */
    /* ES8316 DAC attenuation: 0.5 dB/register step; 0 is unity gain. */
    U(0x33,0xff,attenuation_db*2); U(0x34,0xff,attenuation_db*2);
    U(0x0d,0x19,0);                 /* DAC reference, bias and analog power */
    pause("essxbias",MAX(1,hz/30));
    U(0x14,0xcc,0x88); U(0x15,0x33,0);
    U(0x18,0xbb,0x11);              /* calibration on, attenuated HP drive */
    U(0x19,0x04,0); U(0x1a,0x20,0);
    U(0x17,0x66,0x66); U(0x2f,0x11,0);
    pause("essxanalog",MAX(1,hz/20));
    device_printf(c->dev,"codec prepared: I2S 48k/24-bit, muted\n");
    goto out;
failed:
    (void)codec_rollback(c);
out:
    sx_xunlock(&codec_lock);return(error);
#undef U
}

int
essx_codec_output(int level)
{
    int error;
    sx_xlock(&codec_lock);
    if(!codec||!codec->active) {error=ENXIO;goto out;}
    if(codec->micbias_trial&&!codec->bias_playback) {error=EBUSY;goto out;}
    error=codec_update(codec,0x30,0x20,0x20);
    if(!error) error=pad_set(level);
    if(!error) {pause("essxamp",MAX(1,7*hz/100));error=codec_update(codec,0x30,0x20,0);}
out:
    sx_xunlock(&codec_lock);return(error);
}

int
essx_codec_restore(void)
{
    int error;
    sx_xlock(&codec_lock);error=codec?codec_rollback(codec):0;sx_xunlock(&codec_lock);
    return(error);
}

/* Linux's machine driver shares HPOL/HPOR between the two endpoints.
 * Board wiring still needs listener confirmation. Mode 2 is a temporary
 * diagnostic with the speaker amplifier enabled; it may sound from both.
 * The ES8336D 0x18 gain fields are set to unity for headphone tests, while
 * keeping DAC attenuation >=18 dB. Speaker mode keeps its proven -12 dB
 * analog setting. No supply voltages, mic bias or GPIO173 writes are made.
 */
int
essx_codec_route(int speaker_level,unsigned attenuation_db,int headphones)
{
    struct essx_codec *c;
    unsigned attenuation;
    int error;
    if((speaker_level!=0&&speaker_level!=1)||attenuation_db>48||
        (headphones<0||headphones>2))return(EINVAL);
    attenuation=headphones?MAX(attenuation_db,18u):attenuation_db;
    sx_xlock(&codec_lock);c=codec;
    if(!c||!c->active) {error=ENXIO;goto out;}
    if(c->micbias_trial&&!c->bias_playback) {error=EBUSY;goto out;}
    error=codec_update(c,0x30,0x20,0x20);
    if(!error)error=codec_update(c,0x33,0xff,attenuation*2);
    if(!error)error=codec_update(c,0x34,0xff,attenuation*2);
    if(!error)error=codec_update(c,0x18,0x33,headphones?0:0x11);
    if(!error)error=pad_set(headphones==1?0:(uint32_t)speaker_level);
    if(!error) {
        pause("essxroute",MAX(1,7*hz/100));
        error=codec_update(c,0x30,0x20,0);
    }
    if(error) {
        /* Even an error after the unmute write must try to mute again. */
        (void)codec_update(c,0x30,0x20,0x20);
        device_printf(c->dev,"output switch failed; mute requested errno=%d\n",error);
    } else device_printf(c->dev,"output=%s amp=%d DAC attenuation=%udB\n",
        headphones?"headphones-test":"speakers",headphones==1?0:speaker_level,attenuation);
out:
    sx_xunlock(&codec_lock);return(error);
}

/* ES8336D rev3.0 0x4d bit0: 0=HPInserted input, 1=ADCLRCK I/O.
 * Select the documented detection input without enabling an unclaimed IRQ.
 * Preserve polarity/debounce and all unrelated bits; rollback owns both regs.
 */
int
essx_codec_jack_poll_prepare(void)
{
    int error;
    sx_xlock(&codec_lock);
    if(!codec||!codec->active)error=ENXIO;
    else if(codec->jack_detect_enabled)error=EBUSY;
    else {
        error=codec_update(codec,0x4e,0x02,0); /* IRQ disabled; polling only */
        if(!error)error=codec_update(codec,0x4d,0x01,0);
        if(!error)pause("essxjack",MAX(1,hz/2)); /* longest documented debounce */
        else (void)codec_update(codec,0x30,0x20,0x20);
    }
    sx_xunlock(&codec_lock);return(error);
}

/* Bounded, read-only whitelist. Never expose arbitrary register writes. */
int
essx_codec_snapshot(char *text,size_t length)
{
    static const uint8_t regs[]={0x00,0x01,0x08,0x0c,0x0d,0x13,0x14,0x15,
        0x16,0x17,0x18,0x19,0x1a,0x1b,0x2f,0x30,0x31,0x33,0x34,0x4d,0x4e,0x4f};
    uint8_t value;
    size_t used=0;
    int error=0,n;
    if(text==NULL||length<1)return(EINVAL);
    text[0]='\0';
    sx_xlock(&codec_lock);
    if(!codec||!codec->active) {error=ENXIO;goto out;}
    for(unsigned i=0;i<nitems(regs);i++) {
        error=codec_read(codec,regs[i],&value);if(error)break;
        n=snprintf(text+used,length-used,"%s%02x=%02x",i?" ":"",regs[i],value);
        if(n<0||(size_t)n>=length-used) {error=ENOSPC;break;}
        used+=(size_t)n;
    }
out:
    sx_xunlock(&codec_lock);return(error);
}

/* Raw diagnostic status only. Reading 0x4f is the same register used by
 * Linux v6.12 es8316_irq(); do not assume the HP bit polarity for this board.
 * When explicitly enabled, the host filter is one-shot; a successful
 * status read rearms it outside interrupt context.
 */
int
essx_codec_jack_flags(int *flags)
{
    uint8_t value=0;
    int error;
    if(flags==NULL)return(EINVAL);
    sx_xlock(&codec_lock);
    error=(!codec||!codec->active)?ENXIO:codec_read(codec,JACK_FLAG_REG,&value);
    if(!error&&codec->jack_detect_enabled)error=essx_jack_gpio_rearm();
    if(!error)*flags=value;
    sx_xunlock(&codec_lock);return(error);
}

int
essx_codec_jack_detect_state(void)
{
    int enabled;
    sx_xlock(&codec_lock);enabled=codec&&codec->jack_detect_enabled;
    sx_xunlock(&codec_lock);return(enabled);
}

/* Root-only caller; an actual host IRQ handler must exist before int_en.
 * Preserve debounce and polarity; accept only the known active-high pair. */
int
essx_codec_jack_detect(int enable)
{
    struct essx_codec *c;
    uint8_t config;
    int error=0,e,disable_error;
    if(enable!=0&&enable!=1)return(EINVAL);
    sx_xlock(&codec_lock);c=codec;
    if(!c||!c->active) {error=ENXIO;goto out;}
    if(!enable&&c->micbias_trial) {error=EBUSY;goto out;}
    if(!!enable==c->jack_detect_enabled)goto out;
    if(enable) {
        error=codec_read(c,0x4e,&config);if(error)goto out;
        if(config&0x03) {error=EBUSY;goto out;}
        error=essx_jack_gpio_arm();if(error)goto out;
        c->jack_detect_enabled=true;
        error=codec_update(c,0x4e,0x02,0x02);
        if(!error)error=essx_jack_gpio_rearm();
        if(!error)goto out;
    }
    disable_error=codec_update(c,0x4e,0x02,0);
    e=essx_jack_gpio_disarm();if(!disable_error)disable_error=e;
    if(!disable_error)c->jack_detect_enabled=false;
    if(disable_error&&!error)error=disable_error;
    if(error)(void)codec_update(c,0x30,0x20,0x20);
out:
    sx_xunlock(&codec_lock);return(error);
}

int
essx_codec_micbias_state(void)
{
    int enabled;
    sx_xlock(&codec_lock);enabled=codec&&codec->micbias_trial;
    sx_xunlock(&codec_lock);return(enabled);
}

/* Linux es8316 enables Bias/Analog power/Mic Bias for mic-ground detection.
 * Playback has already enabled Bias and Analog power (0x0d bits3/4=0).
 * This bounded trial toggles only pdnMic bit5, with no voltage selection,
 * ADC stream, pin direction or gain changes. Mute before either transition.
 */
int
essx_codec_micbias_trial(int enable)
{
    struct essx_codec *c;
    uint8_t bias,mute;
    int error=0,restore_error,e;
    if(enable!=0&&enable!=1)return(EINVAL);
    sx_xlock(&codec_lock);c=codec;
    if(!c||!c->active) {error=ENXIO;goto out;}
    if(!enable&&c->bias_playback) {error=EBUSY;goto out;}
    if(!!enable==c->micbias_trial)goto out;
    if(enable) {
        if(!c->jack_detect_enabled) {error=EBUSY;goto out;}
        error=codec_read(c,0x0d,&bias);if(error)goto out;
        error=codec_read(c,0x30,&mute);if(error)goto out;
        /* Refuse an already-on or unpowered baseline: it would not be the
         * controlled OFF/ON comparison this diagnostic claims to perform. */
        if((bias&0x38)!=0x20) {error=EBUSY;goto out;}
        c->trial_bias_original=bias&0x20;c->trial_mute_original=mute&0x20;
        c->micbias_trial=true;
        error=codec_update(c,0x30,0x20,0x20);
        if(!error)error=codec_update(c,0x0d,0x20,0);
        if(!error) {pause("essxmicbias",MAX(1,hz/50));goto out;}
    }
    /* On disable, leave DAC muted until bias restoration has succeeded. */
    restore_error=codec_update(c,0x30,0x20,0x20);
    e=codec_update(c,0x0d,0x20,c->trial_bias_original);
    if(!restore_error)restore_error=e;
    if(!restore_error) {
        pause("essxmicbias",MAX(1,hz/50));
        restore_error=codec_update(c,0x30,0x20,c->trial_mute_original);
    }
    if(!restore_error)c->micbias_trial=false;
    else (void)codec_update(c,0x30,0x20,0x20);
    if(restore_error&&!error)error=restore_error;
out:
    sx_xunlock(&codec_lock);return(error);
}

int
essx_codec_bias_playback_state(void)
{
    int enabled;
    sx_xlock(&codec_lock);enabled=codec&&codec->bias_playback;
    sx_xunlock(&codec_lock);return(enabled);
}

/* Explicit audio permission while holding the validated bias trial.
 * This adds no automatic routing or background kernel worker. Userspace
 * may call the existing muted/attenuated route operation after admission.
 */
int
essx_codec_bias_playback(int enable)
{
    struct essx_codec *c;
    int error=0;
    if(enable!=0&&enable!=1)return(EINVAL);
    sx_xlock(&codec_lock);c=codec;
    if(!c||!c->active) {error=ENXIO;goto out;}
    if(!!enable==c->bias_playback)goto out;
    if(!c->micbias_trial||!c->jack_detect_enabled) {error=EBUSY;goto out;}
    error=codec_update(c,0x30,0x20,enable?0:0x20);
    if(!error)c->bias_playback=!!enable;
    else (void)codec_update(c,0x30,0x20,0x20);
out:
    sx_xunlock(&codec_lock);return(error);
}

int
essx_codec_jack_pin(int *level)
{
    uint32_t number,dw;
    int error;
    if(level==NULL)return(EINVAL);
    sx_xlock(&codec_lock);
    if(!codec||!codec->active) {error=ENXIO;goto out;}
    error=pad_get_for(JACK_IRQ_PAD,"\\_SB.GNUM",&number);
    if(error)goto out;
    if(number!=173) {error=ENXIO;goto out;}
    error=pad_get_for(JACK_IRQ_PAD,"\\_SB.GPC0",&dw);
    if(error)goto out;
    /* Reviewed ACPI getters only; insist on GPIO mode, TX disabled and
     * RX enabled. GPIO173 is an input/IRQ and is never driven here. */
    if(((dw>>10)&7)!=0||(dw&(1u<<8))==0||(dw&(1u<<9))!=0) {
        error=ENXIO;goto out;
    }
    *level=(int)((dw>>1)&1);
out:
    sx_xunlock(&codec_lock);return(error);
}

#ifndef ESSX_CODEC_TEST
static int
codec_probe(device_t dev)
{
    ACPI_HANDLE h=acpi_get_handle(dev);
    ACPI_BUFFER b={ACPI_ALLOCATE_BUFFER,NULL};
    ACPI_OBJECT *o;
    int error=ENXIO;
    if(h==NULL||!acpi_MatchHid(h,"ESSX8336")||iicbus_get_addr(dev)!=0x11) return(ENXIO);
    if(ACPI_SUCCESS(AcpiEvaluateObject(h,"_SUB",NULL,&b))) {
        o=b.Pointer;
        if(o&&o->Type==ACPI_TYPE_STRING&&o->String.Length==8&&
            !memcmp(o->String.Pointer,"70061E50",8))error=BUS_PROBE_DEFAULT;
    }
    if(b.Pointer)AcpiOsFree(b.Pointer);
    if(error==BUS_PROBE_DEFAULT)device_set_desc(dev,"Casper ES8336 experimental codec control");
    return(error);
}
static int
codec_attach(device_t dev)
{
    struct essx_codec *c=device_get_softc(dev);
    sx_xlock(&codec_lock);
    if(codec) {sx_xunlock(&codec_lock);return(EBUSY);}
    c->dev=dev;codec=c;sx_xunlock(&codec_lock);
    device_printf(dev,"attached; codec/GPIO writes require explicit root playback enable\n");
    return(0);
}
static int
codec_detach(device_t dev)
{
    sx_xlock(&codec_lock);
    if(codec&&codec->dev==dev&&codec->active) {sx_xunlock(&codec_lock);return(EBUSY);}
    if(codec&&codec->dev==dev)codec=NULL;
    sx_xunlock(&codec_lock);return(0);
}
static device_method_t codec_methods[]={
    DEVMETHOD(device_probe,codec_probe),DEVMETHOD(device_attach,codec_attach),
    DEVMETHOD(device_detach,codec_detach),DEVMETHOD_END};
static driver_t codec_driver={.name="essxcodec",.methods=codec_methods,.size=sizeof(struct essx_codec)};
DRIVER_MODULE(essxcodec,iicbus,codec_driver,NULL,NULL);
#endif
