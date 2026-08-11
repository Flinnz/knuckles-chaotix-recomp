/* The audio path: the 32X's PWM unit, and the sink everything it plays out
 * goes to. The Mega Drive's YM2612 and PSG will mix into the same sink. */
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

/* The three PWM sample ports, as the register block orders them. */
#define PWM_L    0
#define PWM_R    1
#define PWM_MONO 2

/* A word pushed into one of the FIFOs, and the status word a read of the same
 * address returns — bit 15 full, bit 14 empty. `src` is which CPU wrote it,
 * 'M', 'S' or '6', which the trace carries because the reference's own logs are
 * per-CPU and only the slave's stream can be compared against one. */
void     sound_pwm_write(unsigned which, uint16_t v, char src);
uint16_t sound_pwm_status(unsigned which);

/* Advance the PWM cycle clock by that many SH-2 cycles. One cycle takes a
 * sample out of each FIFO and plays it; every TM of them raises the slave's
 * timer interrupt, which is the only clock that CPU has. */
void sound_pwm_tick(unsigned sh2_cycles);

/* Open the sink. `wav` names a file to write the machine's own sample stream
 * to, at its own rate, and is what a headless run has instead of a device;
 * `device` asks for an SDL audio device as well. Either may be absent. */
int  sound_open(const char *wav, const char *trace, int device);
void sound_close(void);

/* Hold the emulator back while the device still has more than a few frames of
 * audio buffered, which makes the audio clock the frame clock. Does nothing
 * without a device, so a headless run is not slowed by it. */
void sound_pace(void);

/* What came out, for the end-of-run report. */
void sound_report(void);

#endif
