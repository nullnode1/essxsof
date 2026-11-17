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
static const uint8_t codec_regs[]={0x30,0x00,0x0c,0x03,0x01,0x09,0x0b,
    0x06,0x07,0x33,0x34,0x0d,0x14,0x15,0x18,0x19,0x1a,0x17,0x2f};
struct essx_codec {
    device_t dev;
    uint8_t saved[nitems(codec_regs)];
    bool touched[nitems(codec_regs)], active;
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
pad_get(const char *path,uint32_t *v)
{
    ACPI_OBJECT in={.Integer={.Type=ACPI_TYPE_INTEGER,.Value=AMP_PAD}},out={0};
    ACPI_OBJECT_LIST args={.Count=1,.Pointer=&in};
    ACPI_BUFFER buf={.Length=sizeof(out),.Pointer=&out};
    ACPI_STATUS s=AcpiEvaluateObject(NULL,__DECONST(char *,path),&args,&buf);
    if(ACPI_FAILURE(s)||out.Type!=ACPI_TYPE_INTEGER||out.Integer.Value>UINT32_MAX)
        return(EIO);
    *v=(uint32_t)out.Integer.Value;
    return(0);
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
    /* Best effort on every element even if an earlier write fails. */
    e=codec_update(c,0x30,0x20,0x20); if(e)error=e;
    e=pad_set(c->pad_original); if(e)error=e;
    pause("essxmute",MAX(1,hz/50));
    for(i=nitems(codec_regs);i>0;i--) if(c->touched[i-1]) {
        e=codec_write(c,codec_regs[i-1],c->saved[i-1]); if(e)error=e;
    }
    if(!error) {c->active=false;device_unbusy(c->dev);}
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
