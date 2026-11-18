/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSXSOF_H
#define ESSXSOF_H
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/lock.h>
#include <sys/sx.h>
#include <sys/rman.h>
#include <machine/bus.h>
#include <dev/pci/pcivar.h>
#include <sys/endian.h>
#define CONFIG_IPC_MAJOR_3 1
#ifndef BIT
#define BIT(n) (1u << (n))
#endif
#include <ipc/header.h>
#include <ipc/topology.h>
#include <ipc/stream.h>
#include <ipc/dai.h>

#include "essx_wire.h"

struct essx_dma {
    bus_dma_tag_t tag;
    bus_dmamap_t map;
    void *vaddr;
    bus_addr_t paddr;
    bool loaded;
};
struct essx_softc {
    device_t dev;
    struct resource *hda, *dsp;
    struct sx lock;
    struct essx_dma dma;
    struct essx_image image;
    const struct firmware *firmware;
    device_t pcm_child;
    uint32_t pgctl, cgctl, em2;
    uint8_t ltrp;
    bool saved, powered, poisoned, detaching;
    bool pcm_enabled, streaming;
    int amp_gpio;
    int dac_attenuation;
    int output_mode;
    char driver_version[24];
    int phase, last_error;
    const char *stage;
};
struct essx_bdle { uint32_t lo, hi, len, ioc; };
static inline uint32_t er4(struct resource *r, bus_size_t o) { return bus_read_4(r,o); }
static inline void ew4(struct resource *r, bus_size_t o, uint32_t v) { bus_write_4(r,o,v); }

int essx_parse_image(const uint8_t *, size_t, struct essx_image *);
int essx_make_topology(struct essx_softc *);
int essx_ipc(struct essx_softc *, const void *, size_t, const char *);
int essx_stream_command(struct essx_softc *, uint32_t, const char *);
int essx_codec_available(void);
int essx_codec_prepare(unsigned);
int essx_codec_output(int);
int essx_codec_restore(void);
int essx_codec_route(int, unsigned, int);
int essx_codec_jack_flags(int *);
int essx_codec_jack_pin(int *);
int essx_codec_jack_poll_prepare(void);
int essx_codec_snapshot(char *, size_t);
int essx_codec_jack_detect(int);
int essx_codec_jack_detect_state(void);
int essx_codec_micbias_trial(int);
int essx_codec_micbias_state(void);
int essx_codec_bias_playback(int);
int essx_codec_bias_playback_state(void);
int essx_jack_gpio_arm(void);
int essx_jack_gpio_rearm(void);
int essx_jack_gpio_disarm(void);
int essx_jack_gpio_snapshot(char *, size_t);
int essx_pcm_arm(device_t, bool);
int essx_pcm_halt(device_t);
int essx_pcm_position(struct essx_softc *, uint32_t *);
#endif
