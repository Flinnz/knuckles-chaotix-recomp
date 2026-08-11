/* The Mega Drive's PSG — an SN76489 inside the VDP.
 *
 * Three square-wave tone channels and one noise channel, one 4-bit attenuator
 * each, driven by a single write port. It is mono, and on a 32X its output is
 * mixed with the adapter's PWM in the analogue domain, which is what
 * src/sound.c does with `psg_level`.
 */
#ifndef PSG_H
#define PSG_H

#include <stdint.h>

typedef struct {
    uint16_t period[4];      /* 10 bits; channel 3's is a 3-bit noise control */
    uint8_t  vol[4];         /* attenuation, 0 loudest and 15 silent */
    uint16_t count[4];       /* the down-counters, in PSG ticks */
    uint8_t  flip[4];        /* which half of the square each channel is in */
    uint16_t lfsr;           /* the noise shift register */
    uint8_t  latch;          /* which register an unlatched data byte extends */
    unsigned div;            /* the divide-by-16 in front of the counters */

    /* Every tick's output summed since the last sample was taken, which makes
     * reading it a box filter rather than a point sample — the counters run at
     * 224 kHz and the sink asks for 22, so anything else is aliasing. */
    int32_t  acc;
    unsigned acc_n;

    uint32_t writes;         /* for the report */
} PSG;

void psg_reset(PSG *p);
void psg_write(PSG *p, uint8_t v);

/* Advance by that many PSG clocks — the same clock the Z80 has, master over
 * fifteen. */
void psg_tick(PSG *p, unsigned clocks);

/* The average level since the last call, and start a new interval. Zero when
 * every channel is attenuated to silence, which is what this game does. */
int  psg_level(PSG *p);

/* Whether anything at all is audible, for the end-of-run report: a run where
 * the driver never lifts an attenuator looks exactly like a broken core. */
int  psg_audible(const PSG *p);

#endif
