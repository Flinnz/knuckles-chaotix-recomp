/* The 32X's PWM unit, and where the samples it plays out actually go.
 *
 * One CPU feeds it. The slave SH-2's sound driver lives in the cache array at
 * 0xC0000000, takes the PWM timer interrupt, and every second one mixes four
 * channels of 8-bit PCM into two stereo pairs which it pushes into the left and
 * right FIFOs at 0x20004034 and 0x20004036. The reference never shows the
 * master touching either address in 5.6 million instructions.
 *
 * Three things about the unit are pinned by that driver rather than assumed:
 *
 *   *The FIFOs are three words deep.* Its init at 0xC0000028-0xC0000032 writes
 *   exactly three words to each before anything is running, which is what
 *   filling a FIFO looks like; and having filled them it never once finds one
 *   full afterwards — the branch at 0xC00000DA skips the wait at 0xC00000DC in
 *   all 22,612 mixes the reference logs, which only holds if the unit is taking
 *   a sample out per cycle while the driver puts two in every second cycle.
 *
 *   *A sample is a pulse width, and the driver's centre is not the cycle's.*
 *   Its output stage is `xor #0x400 / and #0x7FF / sub #0x200`, so the words it
 *   writes run 0x000-0x3FF about a centre of 0x200, against a cycle register of
 *   0x417 whose own midpoint is 523. Silence therefore sits 11 ticks below the
 *   midpoint — a 2% DC offset, which on the real machine the coupling capacitor
 *   on the audio output removes and which `dcblock` below removes here.
 *
 *   *There is nothing to hear yet.* Every one of those 22,612 left samples is
 *   0x200 exactly. Over the whole 2.06 seconds of PWM output the reference logs
 *   have, the 32X plays silence: the sound in the opening is the Mega Drive's
 *   YM2612 and PSG, which the Z80 is driving and which nothing here models.
 *   That is what `tools/diffpwm.py` holds us to — the same stream, sample for
 *   sample, including its being silent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>
#include "mars.h"
#include "sound.h"

/* ------------------------------------------------------------- the FIFOs ---
 *
 * Three words each, and a read of the same address gives the two flags the
 * driver looks at: bit 15 full, bit 14 empty.
 */
#define PWM_FIFO_DEPTH 3

typedef struct {
    uint16_t q[PWM_FIFO_DEPTH];
    unsigned n;
    uint16_t held;           /* the last sample taken out, played on while dry */
} Fifo;

static Fifo pwm[2];          /* PWM_L, PWM_R */

static uint32_t pwm_writes[2], pwm_dropped[2], pwm_played, pwm_starved;
/* What the stream looks like, which is how a run says whether the 32X made any
 * sound at all: a constant is silence whatever value it holds, so the count
 * that matters is how often the value changes. */
static uint16_t pwm_lo[2] = { 0xFFFF, 0xFFFF }, pwm_hi[2], pwm_last[2];
static uint32_t pwm_changes[2];
static int      pwm_seen[2];

static FILE *pwm_trace;      /* one line per FIFO write, for tools/diffpwm.py */

void sound_pwm_write(unsigned which, uint16_t v, char src) {
    if (which == PWM_MONO) {
        sound_pwm_write(PWM_L, v, src);
        sound_pwm_write(PWM_R, v, src);
        return;
    }
    Fifo *f = &pwm[which];
    if (pwm_trace)
        fprintf(pwm_trace, "%c %04X %c\n", which == PWM_L ? 'L' : 'R', v, src);
    pwm_writes[which]++;
    if (v < pwm_lo[which]) pwm_lo[which] = v;
    if (v > pwm_hi[which]) pwm_hi[which] = v;
    if (pwm_seen[which] && v != pwm_last[which]) pwm_changes[which]++;
    pwm_last[which] = v; pwm_seen[which] = 1;
    /* A write to a full FIFO is lost on hardware, and counting that is the
     * whole point of modelling the depth: a sink that accepted everything could
     * never tell a driver that keeps up from one that does not. */
    if (f->n >= PWM_FIFO_DEPTH) { pwm_dropped[which]++; return; }
    f->q[f->n++] = v;
}

uint16_t sound_pwm_status(unsigned which) {
    if (which == PWM_MONO)
        return (uint16_t)(((pwm[0].n >= PWM_FIFO_DEPTH || pwm[1].n >= PWM_FIFO_DEPTH)
                           ? 0x8000u : 0u)
                        | ((!pwm[0].n && !pwm[1].n) ? 0x4000u : 0u));
    const Fifo *f = &pwm[which];
    return (uint16_t)((f->n >= PWM_FIFO_DEPTH ? 0x8000u : 0u)
                    | (f->n ? 0u : 0x4000u));
}

static uint16_t fifo_take(Fifo *f) {
    if (!f->n) { pwm_starved++; return f->held; }
    f->held = f->q[0];
    memmove(f->q, f->q + 1, --f->n * sizeof *f->q);
    return f->held;
}

/* ---------------------------------------------------------------- the sink --
 *
 * Two consumers, and neither is required: a WAV file, which is what a headless
 * run has and which is the machine's own stream at the machine's own rate, and
 * an SDL device, which needs the stream resampled to whatever rate the host
 * gave us.
 */
#define RING 8192            /* device-rate stereo frames; a third of a second */
static int16_t ring[RING][2];
static SDL_atomic_t ring_w, ring_r;
static SDL_AudioDeviceID dev;
static int dev_rate, had_dev;
static uint32_t under, over;
static int16_t last_out[2];

static FILE *wav;
static uint32_t wav_frames;

/* Linear interpolation state: where the output clock stands inside the current
 * input sample, in units where one input sample is `dev_rate` wide. */
static int rs_acc;
static int16_t rs_prev[2];
static int rs_started;

/* The coupling capacitor on the real output, one pole per channel. At the
 * 22 kHz this game runs at, 0.9995 puts the corner at 1.8 Hz — low enough to
 * leave everything audible alone and high enough to take out the driver's
 * 2% offset from the cycle's midpoint before it reaches a speaker. */
static float hp_x[2], hp_y[2];
static int dcblock(int ch, int s) {
    hp_y[ch] = (float)s - hp_x[ch] + 0.9995f * hp_y[ch];
    hp_x[ch] = (float)s;
    int v = (int)hp_y[ch];
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* A pulse width is a duty cycle, so the midpoint of the cycle is silence. */
static int pwm_level(uint16_t w) {
    unsigned cyc = mars.pwm_cycle & 0xFFF;
    if (cyc < 2) return 0;
    int half = (int)(cyc / 2);
    long s = ((long)(w & 0xFFFu) - half) * 32767 / half;
    return s > 32767 ? 32767 : s < -32768 ? -32768 : (int)s;
}

static void ring_push(int16_t l, int16_t r) {
    int w = SDL_AtomicGet(&ring_w), n = (w + 1) % RING;
    if (n == SDL_AtomicGet(&ring_r)) { over++; return; }
    ring[w][0] = l; ring[w][1] = r;
    SDL_AtomicSet(&ring_w, n);
}

static void SDLCALL sound_mix(void *ud, Uint8 *stream, int len) {
    (void)ud;
    int16_t *out = (int16_t *)stream;
    int frames = len / (int)(2 * sizeof(int16_t));
    int r = SDL_AtomicGet(&ring_r);
    for (int i = 0; i < frames; i++) {
        if (r == SDL_AtomicGet(&ring_w)) under++;      /* dry: hold the last */
        else { last_out[0] = ring[r][0]; last_out[1] = ring[r][1]; r = (r + 1) % RING; }
        out[2 * i] = last_out[0];
        out[2 * i + 1] = last_out[1];
    }
    SDL_AtomicSet(&ring_r, r);
}

/* One sample of the machine's own stream, at the machine's own rate. */
static void play(uint16_t lw, uint16_t rw) {
    unsigned mode = mars.pwm_ctl;
    /* LMD and RMD, bits 1-0 and 3-2: zero is the output switched off. */
    int l = (mode & 3) ? dcblock(0, pwm_level(lw)) : 0;
    int r = ((mode >> 2) & 3) ? dcblock(1, pwm_level(rw)) : 0;

    pwm_played++;
    if (wav) {
        uint8_t b[4] = { (uint8_t)(l & 0xFF), (uint8_t)((l >> 8) & 0xFF),
                         (uint8_t)(r & 0xFF), (uint8_t)((r >> 8) & 0xFF) };
        fwrite(b, 1, 4, wav);            /* WAV is little-endian by definition */
        wav_frames++;
    }
    if (!dev) return;

    unsigned rate = mars_pwm_rate();
    if (!rate) return;
    if (!rs_started) { rs_prev[0] = (int16_t)l; rs_prev[1] = (int16_t)r; rs_started = 1; }
    /* Emit every output sample whose instant falls inside this input interval,
     * interpolated between the last input sample and this one. */
    while (rs_acc < dev_rate) {
        int a = rs_acc;
        ring_push((int16_t)(rs_prev[0] + (long)(l - rs_prev[0]) * a / dev_rate),
                  (int16_t)(rs_prev[1] + (long)(r - rs_prev[1]) * a / dev_rate));
        rs_acc += (int)rate;
    }
    rs_acc -= dev_rate;
    rs_prev[0] = (int16_t)l; rs_prev[1] = (int16_t)r;
}

/* ------------------------------------------------- the Mega Drive's chips --
 *
 * Not synthesised — this is where a YM2612 and a PSG core will go. What it does
 * today is latch the register file and count, which is enough to say whether
 * the driver is doing anything and to compare the traffic against the
 * reference's: over its extract the Z80 makes 95 register writes to the YM's
 * first half, 77 to its second and 40 to the PSG, and the 68000 silences all
 * four PSG channels itself during its init.
 */
static uint8_t ym_reg[2][256];       /* the two halves, latched */
static uint8_t ym_addr[2];
static uint32_t ym_writes[2], psg_writes, psg_from68k;
static uint8_t psg_latch;

void sound_ym_write(unsigned port, uint8_t v) {
    unsigned half = (port >> 1) & 1;
    if (port & 1) { ym_reg[half][ym_addr[half]] = v; ym_writes[half]++; }
    else ym_addr[half] = v;
}

void sound_psg_write(uint8_t v) {
    psg_writes++;
    /* A byte with bit 7 set names a channel and a kind; one without carries the
     * high bits of whatever the last such byte named. Kept so the register
     * file is here when there is something to do with it. */
    if (v & 0x80) psg_latch = v;
}

/* ---------------------------------------------------------- the PWM clock ---
 *
 * The unit takes one sample out of each FIFO per PWM cycle and raises the
 * slave's timer interrupt every TM of them. Both used to be one accumulator in
 * the frame loop counting whole interrupt periods, which is the same thing only
 * while TM is 1 — which is all this game ever programs.
 */
static unsigned pwm_acc, pwm_tm_n;

void sound_pwm_tick(unsigned sh2_cycles) {
    unsigned period = mars_pwm_sample_period();
    if (!period) return;
    unsigned tm = (mars.pwm_ctl >> 8) & 0xF;
    pwm_acc += sh2_cycles;
    while (pwm_acc >= period) {
        pwm_acc -= period;
        play(fifo_take(&pwm[0]), fifo_take(&pwm[1]));
        /* TM zero is the timer interrupt switched off; the sample clock above
         * runs regardless, because the unit plays out whatever it has been
         * given whether or not anyone is being told about it. */
        if (tm && ++pwm_tm_n >= tm) {
            pwm_tm_n = 0;
            mars_deliver_int(1, MARS_INT_PWM);
            mars.pwm_ints++;
        }
    }
}

/* ------------------------------------------------------------ open / close --
 */
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

/* 16-bit stereo PCM at the machine's own sample rate rather than a host one, so
 * nothing here has been resampled — but it has been through `dcblock`, the same
 * way the real output goes through a capacitor. The unfiltered words are what
 * `--trace-pwm` carries, and they are what the gate compares. */
static void wav_header(FILE *f, unsigned rate, uint32_t frames) {
    uint8_t h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                      'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0,0,0,0, 0,0,0,0,
                      4,0, 16,0, 'd','a','t','a', 0,0,0,0 };
    put32(h + 4, 36 + frames * 4);
    put32(h + 24, rate);
    put32(h + 28, rate * 4);
    put32(h + 40, frames * 4);
    fwrite(h, 1, sizeof h, f);
}

int sound_open(const char *wavpath, const char *tracepath, int device) {
    if (tracepath && !(pwm_trace = fopen(tracepath, "w"))) {
        perror(tracepath);
        return 0;
    }
    if (wavpath) {
        if (!(wav = fopen(wavpath, "wb"))) { perror(wavpath); return 0; }
        wav_header(wav, 22000, 0);        /* patched at close with the real rate */
    }
    if (!device) return 1;

    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "audio: %s — running silent\n", SDL_GetError());
        return 1;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 48000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = sound_mix;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
    if (!dev) {
        fprintf(stderr, "audio: %s — running silent\n", SDL_GetError());
        return 1;
    }
    dev_rate = have.freq;
    had_dev = 1;
    SDL_PauseAudioDevice(dev, 0);
    printf("audio: %d Hz, %d-frame buffer\n", dev_rate, have.samples);
    return 1;
}

void sound_close(void) {
    if (dev) { SDL_CloseAudioDevice(dev); dev = 0; }
    if (wav) {
        unsigned rate = mars_pwm_rate();
        fseek(wav, 0, SEEK_SET);
        wav_header(wav, rate ? rate : 22000, wav_frames);
        fclose(wav);
        wav = NULL;
    }
    if (pwm_trace) { fclose(pwm_trace); pwm_trace = NULL; }
}

/* How much the device still has to play, in its own frames. */
static int queued(void) {
    int w = SDL_AtomicGet(&ring_w), r = SDL_AtomicGet(&ring_r);
    return w >= r ? w - r : w + RING - r;
}

void sound_pace(void) {
    /* Four frames of audio buffered is enough that a slow frame does not run
     * the device dry, and little enough that the input still feels immediate.
     * Without a device there is nothing to pace against, which is what keeps a
     * headless run at full speed. */
    if (!dev || !dev_rate) return;
    int high = dev_rate / 15;
    while (queued() > high) SDL_Delay(1);
}

void sound_report(void) {
    printf("  PWM: ctl %04X cycle %04X -> %u Hz, %u int/frame, %u delivered\n",
           mars.pwm_ctl, mars.pwm_cycle, mars_pwm_rate(),
           mars_pwm_per_frame(), mars.pwm_ints);
    printf("       FIFO writes L %u R %u (%u/%u lost to a full FIFO); "
           "%u played, %u starved\n",
           pwm_writes[0], pwm_writes[1], pwm_dropped[0], pwm_dropped[1],
           pwm_played, pwm_starved);
    /* The honest headline. A stream that never changes value is silence
     * whatever that value is, and the reference's whole extract is a constant
     * 0x200 — so a run that says the same here is agreeing with it rather than
     * failing to produce audio. */
    printf("       L %04X-%04X changing %u time(s), R %04X-%04X changing %u\n",
           pwm_lo[0] == 0xFFFF ? 0 : pwm_lo[0], pwm_hi[0], pwm_changes[0],
           pwm_lo[1] == 0xFFFF ? 0 : pwm_lo[1], pwm_hi[1], pwm_changes[1]);
    printf("  YM2612: %u + %u register write(s), last address %02X/%02X; "
           "PSG %u write(s)\n",
           ym_writes[0], ym_writes[1], ym_addr[0], ym_addr[1], psg_writes);
    (void)psg_from68k; (void)psg_latch; (void)ym_reg;
    if (wav_frames)
        printf("       wrote %u frames of PWM-rate audio\n", wav_frames);
    /* Read after sound_close() has stopped the callback thread, which is the
     * only reason these are safe to look at without a lock. */
    if (had_dev)
        printf("       device: %d Hz, %u underrun(s), %u overrun(s)\n",
               dev_rate, under, over);
}
