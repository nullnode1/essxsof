/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef ESSX_JACK_IRQ_H
#define ESSX_JACK_IRQ_H
/* IRQ ownership policy shared by the actual filter and the offline harness.
 * ACPI GPIO173 is GPP_D13 (community1, register group2), not Linux pin173.
 */
#define ESSX_JACK_BIT (1u << 13)
#define ESSX_JACK_IS 0x108u
#define ESSX_JACK_IE 0x128u
static inline bool
essx_jack_pending(bool armed,uint32_t status,uint32_t enabled)
{
    return(armed && (status&enabled&ESSX_JACK_BIT)!=0);
}
static inline uint32_t
essx_jack_mask(uint32_t enabled,bool enable)
{
    return(enable?(enabled|ESSX_JACK_BIT):(enabled&~ESSX_JACK_BIT));
}
static inline bool
essx_jack_input_valid(uint32_t cfg)
{
    /* Preserve the firmware's rising-edge IOAPIC input configuration.
     * No GPIO173 value, direction, event type or polarity writes.
     */
    const uint32_t mask=(3u<<25)|(1u<<23)|(15u<<17)|(15u<<10)|(3u<<8);
    const uint32_t expected=(1u<<25)|(1u<<20)|(1u<<8);
    return(cfg!=UINT32_MAX && (cfg&mask)==expected);
}
#endif
