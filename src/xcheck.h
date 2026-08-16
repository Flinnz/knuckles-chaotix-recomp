/* The two 68000 backends, checked against each other.
 *
 * Every gate in `make check` but one compares against the reference logs, and
 * those are 1.7 seconds of a game that runs for two hundred and fifty. Past
 * them the project's only instrument has been a person playing: the banked
 * window, the divide unit, the eight-bit register and both delay slots were all
 * found that way, and `make check` was byte-identical across every one of them.
 *
 * This is the oracle that needs no reference. Musashi and the recompiler are
 * two independent readings of the same cartridge, so where a translation is
 * wrong they disagree — and unlike the reference they disagree for as long as
 * the game runs.
 *
 * **It is lock-step, not two runs compared.** Running the game twice and
 * diffing the result does not work, and that was measured rather than assumed:
 * at 300 frames the two backends leave byte-identical VDP and 32X state, but
 * over a played session they part company by frame 500 and wander — 1,388 bytes
 * of the 32X dump at 500, 128 at 1,000, 5,561 at 2,000. Nothing is wrong. The
 * recompiler charges a block's cycles in its prologue where Musashi charges
 * each instruction as it retires, so a block that straddles one of the sixteen
 * hand-overs in a scanline overshoots, the SH-2s get a slightly different slice,
 * and a rendezvous resolves an instruction earlier or later. The two runs are
 * the same game at a slightly different point in it, and a gate built on
 * comparing them would report that difference for ever.
 *
 * So the shadow is re-run over exactly the primary's block, from exactly the
 * primary's registers, and timing cannot enter into it. What is compared is
 * what a translation can get wrong: the registers the block leaves behind, the
 * address it goes to next, and every memory access it made on the way.
 */
#ifndef XCHECK_H
#define XCHECK_H

#include <stdint.h>
#include "m68000.h"

/* Off, or one of the two halves of a comparison.
 *
 * RECORD is the ordinary state of a run under `--xcheck`: the recompiled code
 * is the machine, and its data accesses are noted as they happen. REPLAY is the
 * few microseconds in which Musashi re-runs one block, and in it no access
 * reaches the machine at all — reads are answered from the record and writes
 * are checked against it. That is not an optimisation. A read on this machine
 * is not idempotent: reading comm 4 is what hands the cartridge its checksum,
 * the VDP's data port advances its own address, and the HV counter moves every
 * time it is looked at. A shadow that read the machine a second time would be
 * changing what it was measuring.
 */
enum { XCHK_OFF = 0, XCHK_RECORD = 1, XCHK_REPLAY = 2 };
extern int xchk_mode;

/* The recording layer, called from the entry points in src/gen68k.c. */
void     xchk_saw_read(int sz, uint32_t a, uint32_t v);
void     xchk_saw_write(int sz, uint32_t a, uint32_t v);
uint32_t xchk_read(int sz, uint32_t a);
void     xchk_write(int sz, uint32_t a, uint32_t v);

/* Arm on a block entry, and settle the one before it. `off` is the cartridge
 * offset the generated code names, which is the coordinate everything on the
 * recompiled side is in — see the note above xchk_hook in m68000.h. */
void xchk_block(const M68K *c, uint32_t off, unsigned n);

/* Something ran that was not a recompiled block — an interrupt taken between
 * two of them, or a hand-over to the interpreter — so the block still armed is
 * no longer followed by what it would have been followed by. Disarm it. */
void xchk_break(void);

/* Record only while the recompiled 68000 is the thing running.
 *
 * A block that runs out of fuel yields at the *next* block's first line, so it
 * stays armed across the rest of the hand-over — and the machine does not stop
 * there. The Z80 is the one that matters: its bank window is 68000 address
 * space (src/genz80.c reads it through `m68k_read_memory_8`), so the sound
 * driver fetching its next byte out of the cartridge was landing in whichever
 * block happened to be armed, and the shadow was then asked why it had not read
 * an address in the middle of the music. Gating on `m68k_run` is what makes the
 * record mean "what this block did" rather than "what happened while it was
 * waiting to be settled".
 */
void xchk_running(int on);

void xchk_start(int stop_on_first);
void xchk_set_frame(unsigned frame);
void xchk_report(void);
int  xchk_failed(void);

#endif
