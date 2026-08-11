/* The SN76489 in the Mega Drive's VDP.
 *
 * One write port and no readback, so everything it does is decided by a stream
 * of bytes: a byte with bit 7 set latches a register and carries the low four
 * bits of its value, and a byte without carries six more bits into whichever
 * register was latched last. Three tone channels count down from a 10-bit
 * period and toggle, the fourth shifts a register for noise, and each has a
 * 4-bit attenuator where 0 is loudest and 15 is off.
 *
 * The clocks are the whole of the arithmetic. The chip is fed the Z80's
 * 3,579,545 Hz and divides it by 16, so a tone channel that toggles every
 * `period` of those counts comes out at clock / (32 * period) — 285 is the
 * 392 Hz this game's driver writes to channel 0. The sink asks for a sample
 * 22,000 times a second against counters running at 224,000, so what
 * `psg_level` returns is the average since the last one rather than whatever
 * the square happened to be doing at that instant; point-sampling a square wave
 * an order of magnitude above the sample rate is nothing but aliasing.
 *
 * Nothing here is audible in this game's opening. The driver attenuates all
 * four channels to 15 in its init and then writes tone periods for the rest of
 * the reference extract without ever lifting one — 4 volume writes and 36 tone
 * writes across 1.7 seconds — so the music it is playing is muted at the chip.
 * That is a fact about the game and not about this file, and `psg_audible` is
 * what says so out loud in the end-of-run report.
 */
#include "psg.h"

/* Two decibels an attenuation step, and 4,000 at the top so that all four
 * channels at once are half of the output's range rather than all of it. The
 * ceiling is a choice: what a Mega Drive's PSG is worth against a 32X's PWM
 * output is an analogue mixing question the traces cannot answer. */
static const int16_t vol_table[16] = {
    4000, 3177, 2524, 2005, 1592, 1265, 1005, 798,
     634,  503,  400,  318,  252,  201,  159,   0,
};

void psg_reset(PSG *p) {
    for (int i = 0; i < 4; i++) {
        p->period[i] = 0;
        p->vol[i] = 15;              /* silent, which is a chip's reset state */
        p->count[i] = 1;
        p->flip[i] = 0;
    }
    p->lfsr = 0x8000;
    p->latch = 0;
    p->div = 0;
    p->acc = 0;
    p->acc_n = 0;
    p->writes = 0;
}

static void noise_reload(PSG *p) {
    p->lfsr = 0x8000;
}

void psg_write(PSG *p, uint8_t v) {
    p->writes++;
    unsigned ch, type;
    if (v & 0x80) {
        p->latch = (uint8_t)((v >> 4) & 7);
        ch = (v >> 5) & 3;
        type = (v >> 4) & 1;
        if (type) {
            p->vol[ch] = v & 0x0F;
        } else if (ch == 3) {
            p->period[3] = v & 0x0F;
            noise_reload(p);
        } else {
            p->period[ch] = (uint16_t)((p->period[ch] & 0x3F0) | (v & 0x0F));
        }
        return;
    }
    ch = (p->latch >> 1) & 3;
    type = p->latch & 1;
    if (type) p->vol[ch] = v & 0x0F;
    else if (ch == 3) { p->period[3] = v & 0x0F; noise_reload(p); }
    else p->period[ch] = (uint16_t)((p->period[ch] & 0x0F) | ((v & 0x3F) << 4));
}

/* The noise register's low two bits pick how often the shift register moves, in
 * counter reloads: 0x10, 0x20 and 0x40 are the clock over 512, 1024 and 2048,
 * and the fourth setting borrows tone channel 2's period so a driver can sweep
 * the noise's pitch. */
static uint16_t noise_reload_value(const PSG *p) {
    switch (p->period[3] & 3) {
    case 0: return 0x10;
    case 1: return 0x20;
    case 2: return 0x40;
    default: return p->period[2] ? p->period[2] : 1;
    }
}

static void noise_shift(PSG *p) {
    unsigned fb;
    if (p->period[3] & 4) {
        /* White: the parity of the two tapped bits. Periodic: just bit 0, which
         * makes the register cycle rather than run through its whole sequence
         * and so produces a buzz at the shift rate. */
        unsigned x = p->lfsr & 0x0009u;
        fb = 0;
        while (x) { fb ^= x & 1u; x >>= 1; }
    } else {
        fb = p->lfsr & 1u;
    }
    p->lfsr = (uint16_t)((p->lfsr >> 1) | (fb << 15));
}

static int level(const PSG *p) {
    /* Each channel swings between zero and its attenuator's amplitude, which is
     * what the chip does; the DC that leaves is the coupling capacitor's
     * business, and src/sound.c has one. */
    int out = 0;
    for (int i = 0; i < 3; i++)
        if (p->flip[i]) out += vol_table[p->vol[i]];
    if (p->lfsr & 1) out += vol_table[p->vol[3]];
    return out;
}

static void step(PSG *p) {
    for (int i = 0; i < 3; i++) {
        /* A period of zero is a channel a driver has parked: there is nothing
         * to count down, so it holds its output rather than oscillating at half
         * the counter clock. */
        if (!p->period[i]) { p->flip[i] = 1; continue; }
        if (--p->count[i] == 0) {
            p->count[i] = p->period[i];
            p->flip[i] ^= 1;
        }
    }
    if (--p->count[3] == 0) {
        p->count[3] = noise_reload_value(p);
        p->flip[3] ^= 1;
        /* One shift per full cycle of the equivalent square, not per reload. */
        if (p->flip[3]) noise_shift(p);
    }
    p->acc += level(p);
    p->acc_n++;
}

void psg_tick(PSG *p, unsigned clocks) {
    p->div += clocks;
    while (p->div >= 16) {
        p->div -= 16;
        step(p);
    }
    /* Nothing reads the accumulator until the sink has a sample clock, and the
     * 32X's does not start until its driver programs the PWM registers a good
     * fraction of a second in. Decaying rather than growing keeps this bounded
     * without pretending those ticks did not happen. */
    if (p->acc_n >= 8192) { p->acc /= 2; p->acc_n /= 2; }
}

int psg_level(PSG *p) {
    int v = p->acc_n ? (int)(p->acc / (int32_t)p->acc_n) : 0;
    p->acc = 0;
    p->acc_n = 0;
    return v;
}

int psg_audible(const PSG *p) {
    for (int i = 0; i < 4; i++)
        if (p->vol[i] != 15) return 1;
    return 0;
}
