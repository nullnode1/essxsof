/*-
 * SPDX-License-Identifier: GPL-2.0-only
 * Narrow FreeBSD IRQ companion for Casper TN1 001, ACPI INT34C5/GPP_D13.
 * Register definitions and masking/acknowledgement follow Linux v6.12
 * pinctrl-tigerlake.c (Copyright Intel Corporation 2019-2020) and
 * pinctrl-intel.c (Copyright Intel Corporation 2015).
 * Independent FreeBSD bus/lifetime glue; not a general GPIO controller.
 */
#ifdef ESSX_GPIO_TEST
#include "tests/gpio_fake.h"
#else
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/errno.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/sx.h>
#include <sys/systm.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <machine/resource.h>
#include <contrib/dev/acpica/include/acpi.h>
#include <dev/acpica/acpivar.h>
#include "bus_if.h"
#include "device_if.h"
#include "essxsof.h"
#endif
#include "essx_jack_irq.h"

#define JACK_PAD 0x0908000dU
struct essx_jack_gpio {
    device_t dev;
    struct resource *mem[4],*irq;
    void *cookie;
    struct mtx intr_lock;
    int mem_rid[4],irq_rid;
    uint32_t pin_offset,firmware_irq;
    uint64_t interrupts;
    bool armed,irq_eligible;
};
static struct essx_jack_gpio *jack_gpio;
static struct sx jack_gpio_lock;
SX_SYSINIT(essx_jack_gpio_lock,&jack_gpio_lock,"essx jack GPIO lifetime");

#ifndef ESSX_GPIO_TEST
static int
pad_read(const char *name,uint32_t *value)
{
    ACPI_OBJECT in={.Integer={.Type=ACPI_TYPE_INTEGER,.Value=JACK_PAD}},out={0};
    ACPI_OBJECT_LIST args={.Count=1,.Pointer=&in};
    ACPI_BUFFER buffer={.Length=sizeof(out),.Pointer=&out};
    ACPI_STATUS status=AcpiEvaluateObject(NULL,__DECONST(char *,name),&args,&buffer);
    if(ACPI_FAILURE(status)||out.Type!=ACPI_TYPE_INTEGER||out.Integer.Value>UINT32_MAX)
        return(EIO);
    *value=(uint32_t)out.Integer.Value;return(0);
}
#endif

static int
gpio_guard(struct essx_jack_gpio *sc)
{
    uint32_t number,host,owner,cfg;
    if(pad_read("\\_SB.GNUM",&number)||number!=173 ||
        pad_read("\\_SB.GHPO",&host)||host!=1 ||
        pad_read("\\_SB.GGPO",&owner)||owner!=0 ||
        pad_read("\\_SB.GPC0",&cfg)||!essx_jack_input_valid(cfg))return(ENXIO);
    /* Confirm that resource-relative MMIO and the reviewed ACPI getter
     * refer to the same pad. RXSTATE may change between sequential reads. */
    if((bus_read_4(sc->mem[1],sc->pin_offset)^cfg)&~2u)return(ENXIO);
    return(0);
}

/* Called only under intr_lock; preserve every other pin's enable bit. */
static int
gpio_enable_bit(struct essx_jack_gpio *sc,bool enable)
{
    uint32_t current=bus_read_4(sc->mem[1],ESSX_JACK_IE);
    if(current==UINT32_MAX)return(EIO);
    bus_write_4(sc->mem[1],ESSX_JACK_IE,essx_jack_mask(current,enable));
    bus_barrier(sc->mem[1],ESSX_JACK_IE,4,BUS_SPACE_BARRIER_WRITE|BUS_SPACE_BARRIER_READ);
    current=bus_read_4(sc->mem[1],ESSX_JACK_IE);
    return(current==UINT32_MAX||!!(current&ESSX_JACK_BIT)!=enable?EIO:0);
}

static int
gpio_filter(void *context)
{
    struct essx_jack_gpio *sc=context;
    uint32_t status,enabled;
    int result=FILTER_STRAY;
    mtx_lock_spin(&sc->intr_lock);
    status=bus_read_4(sc->mem[1],ESSX_JACK_IS);
    enabled=bus_read_4(sc->mem[1],ESSX_JACK_IE);
    if(status!=UINT32_MAX&&enabled!=UINT32_MAX&&essx_jack_pending(sc->armed,status,enabled)) {
        /* At most one interrupt per successful status-poll rearm. Do not
         * sleep or do I2C/ACPI in this filter. Root polls the codec status. */
        (void)gpio_enable_bit(sc,false);
        bus_write_4(sc->mem[1],ESSX_JACK_IS,ESSX_JACK_BIT); /* W1C this pin only */
        sc->interrupts++;
        result=FILTER_HANDLED;
    }
    mtx_unlock_spin(&sc->intr_lock);
    return(result);
}

/* Do not activate the parent IRQ if another GPIO source is enabled. */
static int
gpio_start_irq(struct essx_jack_gpio *sc)
{
    static const unsigned groups[4]={3,5,5,2};
    int error;
    for(unsigned bank=0;bank<4;bank++)for(unsigned group=0;group<groups[bank];group++) {
        uint32_t enabled=bus_read_4(sc->mem[bank],0x120+group*4);
        if(enabled==UINT32_MAX)return(EIO);
        if(enabled!=0) {
            device_printf(sc->dev,"IRQ setup refused: bank=%u group=%u existing IE=%#x\n",bank,group,enabled);
            return(EBUSY);
        }
    }
    if(sc->cookie)return(0);
    sc->irq=bus_alloc_resource_any(sc->dev,SYS_RES_IRQ,&sc->irq_rid,RF_ACTIVE|RF_SHAREABLE);
    if(!sc->irq)return(ENXIO);
    error=bus_setup_intr(sc->dev,sc->irq,INTR_TYPE_MISC|INTR_MPSAFE,
        gpio_filter,NULL,sc,&sc->cookie);
    if(error) {
        bus_release_resource(sc->dev,SYS_RES_IRQ,sc->irq_rid,sc->irq);
        sc->irq=NULL;sc->cookie=NULL;
    }
    return(error);
}

int
essx_jack_gpio_arm(void)
{
    struct essx_jack_gpio *sc;
    int error;
    uint32_t enabled,status;
    sx_xlock(&jack_gpio_lock);sc=jack_gpio;
    if(!sc||!sc->irq_eligible) {error=ENXIO;goto out;}
    if(sc->armed) {error=EBUSY;goto out;}
    error=gpio_guard(sc);if(error)goto out;
    error=gpio_start_irq(sc);if(error)goto out;
    mtx_lock_spin(&sc->intr_lock);
    enabled=bus_read_4(sc->mem[1],ESSX_JACK_IE);
    status=bus_read_4(sc->mem[1],ESSX_JACK_IS);
    if(enabled==UINT32_MAX||status==UINT32_MAX)error=EIO;
    /* A pre-existing source belongs to someone else: never take it over. */
    else if(enabled&ESSX_JACK_BIT)error=EBUSY;
    else {
        bus_write_4(sc->mem[1],ESSX_JACK_IS,ESSX_JACK_BIT);
        sc->armed=true;error=0; /* stays masked until codec configuration */
    }
    mtx_unlock_spin(&sc->intr_lock);
    if(!error)device_busy(sc->dev);
out:
    sx_xunlock(&jack_gpio_lock);return(error);
}

int
essx_jack_gpio_rearm(void)
{
    struct essx_jack_gpio *sc;
    int error=0;
    sx_xlock(&jack_gpio_lock);sc=jack_gpio;
    if(!sc||!sc->armed)error=ENXIO;
    else {
        mtx_lock_spin(&sc->intr_lock);
        error=gpio_enable_bit(sc,true);
        if(error)(void)gpio_enable_bit(sc,false);
        mtx_unlock_spin(&sc->intr_lock);
    }
    sx_xunlock(&jack_gpio_lock);return(error);
}

int
essx_jack_gpio_disarm(void)
{
    struct essx_jack_gpio *sc;
    int error=0;
    sx_xlock(&jack_gpio_lock);sc=jack_gpio;
    if(sc&&sc->armed) {
        mtx_lock_spin(&sc->intr_lock);
        error=gpio_enable_bit(sc,false); /* original enable bit was zero */
        if(!error) {
            bus_write_4(sc->mem[1],ESSX_JACK_IS,ESSX_JACK_BIT);
            sc->armed=false;
        }
        mtx_unlock_spin(&sc->intr_lock);
        if(!error)device_unbusy(sc->dev);
    }
    sx_xunlock(&jack_gpio_lock);return(error);
}

int
essx_jack_gpio_snapshot(char *text,size_t length)
{
    struct essx_jack_gpio *sc;
    uint32_t status,enabled,cfg;
    uint64_t count;
    bool armed;
    int n,error=0;
    if(text==NULL||length<1)return(EINVAL);
    sx_xlock(&jack_gpio_lock);sc=jack_gpio;
    if(!sc) {error=ENXIO;goto out;}
    mtx_lock_spin(&sc->intr_lock);
    status=bus_read_4(sc->mem[1],ESSX_JACK_IS);enabled=bus_read_4(sc->mem[1],ESSX_JACK_IE);
    cfg=bus_read_4(sc->mem[1],sc->pin_offset);count=sc->interrupts;armed=sc->armed;
    mtx_unlock_spin(&sc->intr_lock);
    n=snprintf(text,length,"INT34C5 mem=%#jx pad_offset=%#x fw_irq=%u irq=%jd eligible=%u handler=%u armed=%u interrupts=%ju IS=%#x IE=%#x PAD=%#x",
        (uintmax_t)rman_get_start(sc->mem[1]),sc->pin_offset,sc->firmware_irq,
        sc->irq?(intmax_t)rman_get_start(sc->irq):(intmax_t)-1,
        sc->irq_eligible,sc->cookie!=NULL,armed,(uintmax_t)count,status,enabled,cfg);
    if(n<0||(size_t)n>=length)error=ENOSPC;
out:
    sx_xunlock(&jack_gpio_lock);return(error);
}

#ifndef ESSX_GPIO_TEST
static int
gpio_probe(device_t dev)
{
    ACPI_HANDLE handle=acpi_get_handle(dev),codec_handle;
    ACPI_BUFFER buffer={ACPI_ALLOCATE_BUFFER,NULL};
    ACPI_OBJECT *sub;
    UINT32 pchs;
    int error=ENXIO;
    if(handle==NULL||!acpi_MatchHid(handle,"INT34C5"))return(ENXIO);
    if(ACPI_FAILURE(acpi_GetInteger(NULL,"\\PCHS",&pchs))||pchs!=2)return(ENXIO);
    if(ACPI_FAILURE(AcpiGetHandle(NULL,"\\_SB.PC00.I2C3.ESSX",&codec_handle)))return(ENXIO);
    if(ACPI_SUCCESS(AcpiEvaluateObject(codec_handle,"_SUB",NULL,&buffer))) {
        sub=buffer.Pointer;
        if(sub&&sub->Type==ACPI_TYPE_STRING&&sub->String.Length==8&&
            !memcmp(sub->String.Pointer,"70061E50",8))error=BUS_PROBE_DEFAULT;
    }
    if(buffer.Pointer)AcpiOsFree(buffer.Pointer);
    if(error==BUS_PROBE_DEFAULT)device_set_desc(dev,"Casper ES8336 GPP_D13 IRQ diagnostic");
    return(error);
}

static int
gpio_attach(device_t dev)
{
    struct essx_jack_gpio *sc=device_get_softc(dev);
    ACPI_OBJECT inputs[2],output={0};
    ACPI_OBJECT_LIST args={.Count=2,.Pointer=inputs};
    ACPI_BUFFER buffer={.Length=sizeof(output),.Pointer=&output};
    rman_res_t irq_start,irq_count;
    static const unsigned first_group[4]={0,6,11,3};
    ACPI_STATUS status;
    UINT32 fw_irq;
    const char *stage="singleton";
    sc->dev=dev;sc->irq_rid=0;sc->firmware_irq=UINT32_MAX;
    mtx_init(&sc->intr_lock,"essx jack IRQ",NULL,MTX_SPIN);
    sx_xlock(&jack_gpio_lock);
    if(jack_gpio)goto fail;
    stage="ACPI memory resources";
    for(unsigned i=0;i<4;i++) {
        sc->mem_rid[i]=(int)i;
        sc->mem[i]=bus_alloc_resource_any(dev,SYS_RES_MEMORY,&sc->mem_rid[i],RF_ACTIVE);
        if(!sc->mem[i]||rman_get_size(sc->mem[i])!=0x10000)goto fail;
    }
    bzero(inputs,sizeof(inputs));
    inputs[0].Integer.Type=inputs[1].Integer.Type=ACPI_TYPE_INTEGER;
    stage="ACPI community mapping";
    for(unsigned i=0;i<4;i++) {
        inputs[0].Integer.Value=first_group[i];inputs[1].Integer.Value=2;
        buffer.Length=sizeof(output);bzero(&output,sizeof(output));
        status=AcpiEvaluateObject(NULL,"\\_SB.GADR",&args,&buffer);
        if(ACPI_FAILURE(status)||output.Type!=ACPI_TYPE_INTEGER||
            output.Integer.Value!=rman_get_start(sc->mem[i])+0x700)goto fail;
    }
    inputs[0].Integer.Value=8;inputs[1].Integer.Value=2;
    buffer.Length=sizeof(output);bzero(&output,sizeof(output));
    status=AcpiEvaluateObject(NULL,"\\_SB.GADR",&args,&buffer);
    if(ACPI_FAILURE(status)||output.Type!=ACPI_TYPE_INTEGER||
        output.Integer.Value!=rman_get_start(sc->mem[1])+0x900)goto fail;
    sc->pin_offset=0x900+13*0x10;
    stage="GPP_D13 input ownership";
    if(gpio_guard(sc))goto fail;
    if(ACPI_SUCCESS(acpi_GetInteger(NULL,"\\SGIR",&fw_irq)))sc->firmware_irq=fw_irq;
    /* Accept only the firmware's actual resource. Never invent IRQ14 or
     * allocate IRQ0 when a resource is missing. Diagnostics still attach. */
    if(bus_get_resource(dev,SYS_RES_IRQ,0,&irq_start,&irq_count)==0&&
        irq_count==1&&irq_start>0&&irq_start<120&&irq_start==sc->firmware_irq) {
        sc->irq_eligible=true;
    }
    jack_gpio=sc;
    device_printf(dev,"read-only until root enables detection; firmware IRQ=%u IRQ-resource=%s pad_offset=%#x\n",
        sc->firmware_irq,sc->irq_eligible?"available":"unavailable",sc->pin_offset);
    sx_xunlock(&jack_gpio_lock);return(0);
fail:
    device_printf(dev,"attach refused: %s; no GPIO register writes performed\n",stage);
    for(unsigned i=0;i<4;i++)if(sc->mem[i])
        bus_release_resource(dev,SYS_RES_MEMORY,sc->mem_rid[i],sc->mem[i]);
    sx_xunlock(&jack_gpio_lock);mtx_destroy(&sc->intr_lock);return(ENXIO);
}

static int
gpio_detach(device_t dev)
{
    struct essx_jack_gpio *sc=device_get_softc(dev);
    int error;
    sx_xlock(&jack_gpio_lock);
    if(sc->armed) {sx_xunlock(&jack_gpio_lock);return(EBUSY);}
    if(sc->cookie) {
        error=bus_teardown_intr(dev,sc->irq,sc->cookie);
        if(error) {sx_xunlock(&jack_gpio_lock);return(error);}
        sc->cookie=NULL;
    }
    if(jack_gpio==sc)jack_gpio=NULL;
    if(sc->irq)bus_release_resource(dev,SYS_RES_IRQ,sc->irq_rid,sc->irq);
    for(unsigned i=0;i<4;i++)if(sc->mem[i])
        bus_release_resource(dev,SYS_RES_MEMORY,sc->mem_rid[i],sc->mem[i]);
    sx_xunlock(&jack_gpio_lock);mtx_destroy(&sc->intr_lock);return(0);
}
static int gpio_suspend(device_t dev) {(void)dev;return(EBUSY);}
static int gpio_shutdown(device_t dev)
{
    (void)dev;return(essx_jack_gpio_disarm());
}
static device_method_t gpio_methods[]={
    DEVMETHOD(device_probe,gpio_probe),DEVMETHOD(device_attach,gpio_attach),
    DEVMETHOD(device_detach,gpio_detach),DEVMETHOD(device_suspend,gpio_suspend),
    DEVMETHOD(device_shutdown,gpio_shutdown),DEVMETHOD_END};
static driver_t gpio_driver={.name="essxjackgpio",.methods=gpio_methods,.size=sizeof(struct essx_jack_gpio)};
DRIVER_MODULE(essxjackgpio,acpi,gpio_driver,NULL,NULL);
#endif
