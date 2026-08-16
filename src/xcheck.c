/* Musashi as a shadow over the recompiled 68000. See xcheck.h for why.
 *
 * One block at a time. When the recompiled build enters a block, the block
 * before it is finished, and everything needed to re-run that one is in hand:
 * the registers it started with, the address it started at, how many
 * instructions it holds, and — because the recording layer below was watching —
 * every data access it made. So Musashi is given the same starting state and
 * run for the same number of instructions, with its reads answered from the
 * record, and what it produces is compared against what the recompiled code
 * produced.
 *
 * The comparison is three things at once, and the third is the one worth the
 * machinery:
 *
 *   * the registers, which catch a translation that computes the wrong value;
 *   * the accesses, in order, which catch one that goes to the wrong address;
 *   * **the address it ends at**, which catches a block that was never the
 *     right code to be running. That is the banked window: 0x900000-0x9FFFFF
 *     shows a different megabyte of the cartridge depending on a register, so
 *     the same address is different code at different moments, and the front
 *     end folded it to bank 0. The 68000's `jsr 0x928EEC` ran bank 0's bytes as
 *     though they were bank 2's for as long as nobody looked. Here the shadow
 *     fetches through the real address and the primary ran the translated
 *     offset, so the two decode different instructions and part on the first
 *     one.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xcheck.h"
#include "m68k.h"

int xchk_mode = XCHK_OFF;
void (*xchk_hook)(const M68K *, uint32_t, unsigned);
void (*xchk_dispatch)(uint32_t, uint32_t);

/* --- the record ----------------------------------------------------------
 *
 * In bytes, not in accesses. The two backends are not obliged to touch memory
 * the same way round and they do not: `movem.l -(a7)` is one 32-bit write per
 * register in the generated code and two 16-bit writes in Musashi, going the
 * other way — the same six bytes at the same six addresses in a different order
 * at a different width. Holding them to the *shape* of their accesses reported
 * that difference on every stack frame the engine builds, which is a fact about
 * two implementations rather than about the cartridge.
 *
 * What both are obliged to agree on is what the block reads and what it leaves
 * behind: the same bytes at the same addresses with the same values. So an
 * access is broken into its bytes as it is recorded, reads are answered per
 * byte, and the writes are compared as a whole at the end — sorted by address,
 * which normalises the order across addresses while keeping it within one, so
 * that a register written twice in a block still has to be written twice in the
 * same sequence.
 */
/* `seq` is where the byte fell in the run that produced it, and it is what makes
 * the sort below stable. That is not tidiness: this engine writes the VDP's
 * control port nine times in one block, so nine bytes share an address, and an
 * unstable sort permutes them differently on the two sides and reports a
 * rotation of the same nine values as nine divergences. */
typedef struct { uint32_t addr; unsigned seq; uint8_t val; } Byte;

/* A basic block here runs to a few dozen instructions, and the widest thing one
 * of them does is a `movem.l` of all sixteen registers — 64 bytes. A block that
 * overflows this is skipped and counted rather than half-checked. */
#define XCHK_LOG_MAX 2048
static Byte rd[XCHK_LOG_MAX];   /* what the block read, in order */
static uint8_t rd_used[XCHK_LOG_MAX];
static unsigned rd_n;
static Byte wr[XCHK_LOG_MAX];   /* what the block wrote, in order */
static unsigned wr_n;
static Byte swr[XCHK_LOG_MAX];  /* and what the shadow wrote */
static unsigned swr_n;
static int log_over;            /* the primary touched more than fits */

/* --- what is armed -------------------------------------------------------- */
static M68K pend;             /* registers at the pending block's entry */
static uint32_t pend_off;     /* the cartridge offset the generated code names */
static unsigned pend_n;       /* the block's instructions */
static int armed;

static unsigned frame;
static int stop_on_first;
static int enabled;

/* --- the tally ------------------------------------------------------------ */
static unsigned long checked, skipped_over, disarmed;
static unsigned long folds, diverged;
static char first[1024];      /* the first divergence, in full */
static unsigned first_frame;

/* Everything a divergence has to say, accumulated across one block's
 * comparison so that a report names every register that differs rather than the
 * first one found. */
static char why[1024];
static unsigned why_n;

static void note(const char *fmt, ...) {
    va_list ap;
    if (why_n >= sizeof why - 1) return;
    va_start(ap, fmt);
    why_n += (unsigned)vsnprintf(why + why_n, sizeof why - why_n, fmt, ap);
    va_end(ap);
    if (why_n >= sizeof why) why_n = sizeof why - 1;
}

/* --- the recording layer -------------------------------------------------- */

/* Big-endian, which is the only order these two ever mean.
 *
 * The address is masked to the twenty-four lines the 68000 actually has, and
 * that is not cosmetic here. The recompiled side hands the memory functions
 * whatever the address arithmetic produced — a `movem.l (a6)` with a6 holding a
 * sign-extended 0xFFFFFFC0 arrives as exactly that — and src/gen68k.c masks it
 * at the door, where Musashi has already masked it going out. Recording the
 * unmasked number would put the same byte of work RAM in the log under two
 * different names. It is the data-side face of the debt docs/roadmap.md carries
 * about the recompiled PC, and the same answer: the bus is 24 bits wide.
 */
static void spread(Byte *v, unsigned *n, int sz, uint32_t a, uint32_t val) {
    if (*n + (unsigned)sz > XCHK_LOG_MAX) { log_over = 1; return; }
    a &= 0xFFFFFFu;
    for (int i = 0; i < sz; i++) {
        v[*n].addr = (a + (uint32_t)i) & 0xFFFFFFu;
        v[*n].seq = *n;
        v[*n].val = (uint8_t)(val >> ((sz - 1 - i) * 8));
        ++*n;
    }
}

void xchk_saw_read(int sz, uint32_t a, uint32_t v) { spread(rd, &rd_n, sz, a, v); }
void xchk_saw_write(int sz, uint32_t a, uint32_t v) { spread(wr, &wr_n, sz, a, v); }

static const char *szname(int sz) {
    return sz == 1 ? "b" : sz == 2 ? "w" : "l";
}

/* The earliest byte of this address the block read and the shadow has not been
 * given yet. Taking them in order is what keeps a register that answers
 * differently each time — the HV counter, the VDP's status — answering the
 * shadow in the sequence the block saw. */
uint32_t xchk_read(int sz, uint32_t a) {
    uint32_t out = 0;
    a &= 0xFFFFFFu;
    for (int i = 0; i < sz; i++) {
        uint32_t want = (a + (uint32_t)i) & 0xFFFFFFu;
        unsigned j;
        for (j = 0; j < rd_n; j++)
            if (!rd_used[j] && rd[j].addr == want) break;
        if (j == rd_n) {
            note("    shadow read%s 0x%06X, and the block never read 0x%06X\n",
                 szname(sz), a, want);
            out <<= 8;
            continue;
        }
        rd_used[j] = 1;
        out = (out << 8) | rd[j].val;
    }
    return out;
}

void xchk_write(int sz, uint32_t a, uint32_t v) { spread(swr, &swr_n, sz, a, v); }

static int by_addr(const void *x, const void *y) {
    const Byte *p = x, *q = y;
    if (p->addr != q->addr) return p->addr < q->addr ? -1 : 1;
    return p->seq < q->seq ? -1 : p->seq > q->seq ? 1 : 0;
}

/* Sorted by address, and within an address by when it was written — so the
 * order the two backends visited *different* addresses in does not matter, and
 * the order they wrote *one* address in does. The first is an implementation
 * detail neither owes the other; the second is the instruction sequence, which
 * they share. */
static void compare_writes(void) {
    qsort(wr, wr_n, sizeof *wr, by_addr);
    qsort(swr, swr_n, sizeof *swr, by_addr);
    unsigned n = wr_n < swr_n ? wr_n : swr_n, said = 0;
    for (unsigned i = 0; i < n && said < 8; i++) {
        if (wr[i].addr != swr[i].addr) {
            note("    block wrote 0x%06X, shadow wrote 0x%06X\n",
                 wr[i].addr, swr[i].addr);
            said++;
            break;
        }
        if (wr[i].val != swr[i].val) {
            note("    0x%06X: block wrote 0x%02X, shadow wrote 0x%02X\n",
                 wr[i].addr, wr[i].val, swr[i].val);
            said++;
        }
    }
    if (wr_n > swr_n)
        note("    %u byte(s) the shadow never wrote, from 0x%06X\n",
             wr_n - swr_n, wr[n].addr);
    else if (swr_n > wr_n)
        note("    %u byte(s) the block never wrote, from 0x%06X\n",
             swr_n - wr_n, swr[n].addr);
}

/* --- the shadow ----------------------------------------------------------- */

/* SR before A7, for the reason src/mars_main.c's to_musashi() gives: M68K_REG_SP
 * is whichever stack pointer the S bit selects, so setting it first would put
 * the value in the other one. */
static void load_shadow(const M68K *c, uint32_t pc) {
    for (unsigned i = 0; i < 8; i++) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), c->d[i]);
        m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), c->a[i]);
    }
    m68k_set_reg(M68K_REG_SR, m68k_get_sr(c));
    m68k_set_reg(M68K_REG_SP, c->a[7]);
    m68k_set_reg(M68K_REG_USP, c->usp);
    m68k_set_reg(M68K_REG_PC, pc);
}

static void compare(const M68K *now, uint32_t now_off) {
    static const char *dn[8] = {"d0","d1","d2","d3","d4","d5","d6","d7"};
    static const char *an[8] = {"a0","a1","a2","a3","a4","a5","a6","a7"};
    for (unsigned i = 0; i < 8; i++) {
        uint32_t s = (uint32_t)m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_D0 + i));
        if (s != now->d[i])
            note("    %s: block 0x%08X, shadow 0x%08X\n", dn[i], now->d[i], s);
    }
    for (unsigned i = 0; i < 7; i++) {
        uint32_t s = (uint32_t)m68k_get_reg(NULL, (m68k_register_t)(M68K_REG_A0 + i));
        if (s != now->a[i])
            note("    %s: block 0x%08X, shadow 0x%08X\n", an[i], now->a[i], s);
    }
    uint32_t sp = (uint32_t)m68k_get_reg(NULL, M68K_REG_SP);
    if (sp != now->a[7])
        note("    a7: block 0x%08X, shadow 0x%08X\n", now->a[7], sp);
    uint16_t sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
    uint16_t nsr = m68k_get_sr(now);
    /* The trace bit is Musashi's to move on an exception and nothing here sets
     * it, and the interrupt mask is changed outside a block by the interrupt
     * itself, so neither is the translation's to get right. The condition codes
     * and the supervisor bit are. */
    if ((sr & 0x201F) != (nsr & 0x201F))
        note("    sr: block 0x%04X, shadow 0x%04X\n", nsr, sr);
    /* The shadow runs in offset space, so a relative branch lands on the same
     * number the generated code does. An *absolute* transfer does not: the
     * cartridge encodes `jsr 0x881868` and the shadow reads that address out of
     * the instruction where the front end folded it to 0x1868 at build time. So
     * the fold is applied to the shadow's answer too — and whether the fold is
     * *true* is the separate question `dispatch` below asks of memory itself,
     * which is what stops this from assuming what it is checking. */
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    if (pc != now_off && canon68k(pc) != now_off)
        note("    went to 0x%06X, shadow went to 0x%06X\n", now_off, pc);
    compare_writes();
    for (unsigned i = 0; i < rd_n; i++)
        if (!rd_used[i]) {
            note("    the block read 0x%06X and the shadow did not\n",
                 rd[i].addr);
            break;
        }
}

/* Report a divergence, once in full and thereafter by counting. */
static void found(const char *what, uint32_t where) {
    diverged++;
    if (diverged > 1) return;
    first_frame = frame;
    snprintf(first, sizeof first, "  frame %u, %s 0x%06X\n%s",
             frame, what, where, why);
    fprintf(stderr, "\n  [xcheck] the two 68000 backends disagree\n%s", first);
    if (!stop_on_first) return;
    fprintf(stderr, "  [xcheck] stopping on the first one\n");
    xchk_report();
    exit(1);
}

/* Re-run the armed block and say whether it came out the same. */
static void verify(const M68K *now, uint32_t now_off) {
    static unsigned char ctx[4096];
    unsigned size = m68k_context_size();
    if (size > sizeof ctx) { armed = 0; return; }

    m68k_get_context(ctx);
    load_shadow(&pend, pend_off);
    /* Nothing may interrupt the shadow: it is re-running instructions that have
     * already been run, and an exception frame it pushed would be a write the
     * block never made. */
    m68k_set_irq(0);

    why_n = 0;
    why[0] = 0;
    swr_n = 0;
    memset(rd_used, 0, rd_n);
    xchk_mode = XCHK_REPLAY;
    /* One cycle is one instruction: m68k_execute's main loop is a do/while on
     * the cycle pool, so it retires an instruction and then finds itself
     * overdrawn. Asking for the block's cycles instead would retire whatever
     * number of instructions that bought, which is not the question. */
    for (unsigned i = 0; i < pend_n; i++) m68k_execute(1);
    xchk_mode = XCHK_RECORD;

    compare(now, now_off);
    m68k_set_context(ctx);

    checked++;
    if (why_n) found("block", pend_off);
}

/* --- the other half: was this the right code to be running? ---------------
 *
 * The shadow above runs at the offset, not at the address, and it has to: the
 * recompiled 68000 works in offsets from end to end — a block returns one, a
 * `bsr` pushes one — so an address only exists at the moment `m68k_run` is
 * handed one. Running the shadow anywhere else would report that difference on
 * every subroutine call in the game and say nothing about the cartridge.
 *
 * But the fold from address to offset is exactly where the worst bug this
 * project has had lived, so it gets a check of its own, here, at the one point
 * both numbers are in hand. `canon68k` claims the bytes at `addr` are the bytes
 * at `off`. The machine can be asked directly — a read at `addr` goes through
 * `rom_at`, which honours the bank register and the mapper — and if the answer
 * differs then the block about to run is not the code at that address.
 *
 * The first word is enough to catch it and is all this can afford: the block's
 * length is not known here, and the banked window's two banks differ from their
 * first instruction (bank 0's bytes decoded as `ori.b #110,(a0)+`). A window
 * whose first word happened to agree and whose second did not would get past
 * this, which is worth knowing and has never been observed.
 */
static void dispatch(uint32_t addr, uint32_t off) {
    folds++;
    /* Through the recording layer's back door: this is the gate asking, not the
     * 68000, so it must not become part of the block's record. */
    int m = xchk_mode;
    xchk_mode = XCHK_OFF;
    uint32_t at = m68k_read_memory_16(addr), of = m68k_read_memory_16(off);
    xchk_mode = m;
    if (at == of) return;
    why_n = 0;
    why[0] = 0;
    note("    the front end runs offset 0x%06X here, which holds 0x%04X, and "
         "0x%06X holds 0x%04X\n", off, of, addr, at);
    found("transfer to", addr);
}

/* --- what the run calls --------------------------------------------------- */

void xchk_block(const M68K *c, uint32_t off, unsigned n) {
    if (xchk_mode != XCHK_RECORD) return;
    if (armed) {
        if (log_over) skipped_over++;
        else verify(c, off);
        armed = 0;
    }
    if (n == 0 || n > XCHK_LOG_MAX) return;
    pend = *c;
    pend_off = off;
    pend_n = n;
    armed = 1;
    rd_n = wr_n = swr_n = 0;
    log_over = 0;
}

void xchk_break(void) {
    if (armed) { armed = 0; disarmed++; }
}

void xchk_running(int on) {
    if (enabled) xchk_mode = on ? XCHK_RECORD : XCHK_OFF;
}

void xchk_start(int stop) {
    /* Musashi banks the 68000's reset cycles and spends them on whatever calls
     * it next, returning before it retires anything. For a shadow run that is
     * one instruction lost out of every block — the first `m68k_execute(1)`
     * does nothing and the comparison ends an instruction short of the branch,
     * which reads exactly like a control-flow divergence and is not one. So
     * spend them here, once, where there is nothing to lose: with no cycle
     * budget the call returns the moment they are eaten.
     *
     * Under `--recomp` Musashi's registers are whatever `to_musashi` last put
     * there and are overwritten at the next hand-over, so even in the case this
     * warns about nothing downstream depends on them. */
    uint32_t pc = (uint32_t)m68k_get_reg(NULL, M68K_REG_PC);
    m68k_execute(0);
    if ((uint32_t)m68k_get_reg(NULL, M68K_REG_PC) != pc)
        fprintf(stderr, "  [xcheck] the interpreter had no reset cycles banked "
                        "and has stepped one instruction\n");
    enabled = 1;
    xchk_hook = xchk_block;
    xchk_dispatch = dispatch;
    stop_on_first = stop;
}

void xchk_set_frame(unsigned f) { frame = f; }

int xchk_failed(void) { return diverged != 0; }

void xchk_report(void) {
    if (xchk_mode == XCHK_OFF && !checked) return;
    printf("  xcheck: %lu block(s) re-run on Musashi, %lu windowed transfer(s) "
           "checked against the fold, %lu divergence(s)\n",
           checked, folds, diverged);
    if (disarmed || skipped_over)
        printf("          %lu not followed by a recompiled block, "
               "%lu over the access log\n", disarmed, skipped_over);
    if (diverged)
        printf("\n  first divergence, at frame %u:\n%s", first_frame, first);
}
