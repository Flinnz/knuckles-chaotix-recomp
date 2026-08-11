| Semantics cases for the 68000 recompiler.
|
| Each case leaves a result in %d0 and calls `record`, which stores the result
| and the condition codes side by side. Musashi runs the same program over the
| same memory, so the comparison is against an independent implementation of the
| same instruction set rather than against numbers written down by hand — which
| is what the flag rules need, since V and C are exactly the part nobody gets
| right from the manual on the first try.
|
| %a6 walks the results, %d6/%d7 are scratch for `record`. Everything else is
| fair game for a case.

    .text
    .globl start
start:
    lea     results,%a6

| --- add / sub: the four flag rules that actually differ -------------------
    move.w  #0x7fff,%d0
    add.w   #1,%d0                  | signed overflow, no carry
    bsr     record

    move.w  #0xffff,%d0
    add.w   #1,%d0                  | carry out, result zero
    bsr     record

    move.w  #0x8000,%d0
    sub.w   #1,%d0                  | signed overflow the other way
    bsr     record

    move.w  #0,%d0
    sub.w   #1,%d0                  | borrow
    bsr     record

    moveq   #100,%d0
    moveq   #100,%d1
    cmp.w   %d1,%d0                 | equal: Z, no C
    bsr     record

    moveq   #-1,%d0
    moveq   #1,%d1
    cmp.b   %d1,%d0                 | 0xff vs 0x01 as bytes
    bsr     record

| --- logic clears V and C, leaves X ---------------------------------------
    move.w  #0xffff,%d0
    add.w   #1,%d0                  | set X and C
    move.w  #0xf0f0,%d0
    and.w   #0x0ff0,%d0
    bsr     record

    move.w  #0x00ff,%d0
    eor.w   #0xff00,%d0             | eor clears V and C
    bsr     record

    move.l  #0x12345678,%d0
    not.l   %d0                    | not is a logic op, X untouched
    bsr     record

| --- shifts and rotates ----------------------------------------------------
    move.w  #0xc001,%d0
    lsl.w   #1,%d0                  | C from the bit shifted out
    bsr     record

    move.w  #0xc001,%d0
    asr.w   #1,%d0                  | sign replicated
    bsr     record

    move.w  #0x4000,%d0
    asl.w   #1,%d0                  | sign changes: V set
    bsr     record

    move.w  #0x4000,%d0
    lsl.w   #1,%d0                  | same bits, V clear
    bsr     record

    move.w  #0x8001,%d0
    rol.w   #1,%d0                 | rotate leaves X alone
    bsr     record

    move.w  #0x8001,%d0
    ror.w   #1,%d0                 | and so does the other way
    bsr     record

    move.w  #0xffff,%d0
    add.w   #1,%d0                  | X = 1
    move.w  #0x0001,%d0
    roxr.w  #1,%d0                  | X rotates in at the top
    bsr     record

    move.w  #0x1234,%d0
    moveq   #0,%d1
    lsr.w   %d1,%d0                 | zero count: C cleared, X kept
    bsr     record

    move.l  #0x00000001,%d0
    moveq   #33,%d1
    lsl.l   %d1,%d0                 | count taken modulo 64, not 32
    bsr     record

| --- extend, swap, sign ----------------------------------------------------
    move.w  #0x00ff,%d0
    ext.w   %d0                    | byte to word
    bsr     record

    move.l  #0x0000ffff,%d0
    ext.l   %d0                    | word to long
    bsr     record

    move.l  #0x12345678,%d0
    swap    %d0                    | halves exchanged, V and C cleared
    bsr     record

    moveq   #-8,%d0                 | sign-extends to the full long
    bsr     record

| --- extended precision ----------------------------------------------------
    move.l  #0xffffffff,%d0
    move.l  #0x00000001,%d1
    add.l   %d1,%d0                 | X = 1, Z = 1
    move.w  #0,%d0
    move.w  #0,%d1
    addx.w  %d1,%d0                 | Z survives only if it was already set
    bsr     record

    moveq   #0,%d0
    subx.w  %d0,%d0                | Z was clear, so it stays clear
    bsr     record

| --- bit operations --------------------------------------------------------
    move.l  #0x00000000,%d0
    bset    #31,%d0                 | register: modulo 32
    bsr     record

    move.l  #0xffffffff,%d0
    bclr    #7,%d0                 | Z from the bit before it changed
    bsr     record

    move.l  #0x0f0f0f0f,%d0
    bchg    #4,%d0                 | bit was 0, so Z is set
    bsr     record

    move.b  #0x55,byte_scratch
    move.l  #0,%d0
    btst    #1,byte_scratch         | memory: modulo 8
    bsr     record

| --- multiply and divide ---------------------------------------------------
    move.w  #-3,%d0
    muls.w  #1000,%d0              | signed, full 32-bit product
    bsr     record

    move.w  #0xfffd,%d0
    mulu.w  #2,%d0                 | unsigned: 65533 x 2, not -3 x 2
    bsr     record

    move.l  #1000,%d0
    divu.w  #7,%d0                  | quotient in the low half, remainder high
    bsr     record

    move.l  #-1000,%d0
    divs.w  #7,%d0                 | signed division truncates toward zero
    bsr     record

| --- conditional set, dbra -------------------------------------------------
    moveq   #0,%d0
    moveq   #1,%d1
    cmp.w   %d1,%d0
    slt     %d0                     | true: 0xff
    bsr     record

    moveq   #0,%d0
    moveq   #5,%d1
1:  addq.w  #1,%d0
    dbf     %d1,1b                  | runs six times
    bsr     record

| --- movem round trip ------------------------------------------------------
    movem.l %d0-%d3,-(%sp)
    move.l  #0x11111111,%d0
    move.l  #0x22222222,%d1
    move.l  #0x33333333,%d2
    move.l  #0x44444444,%d3
    movem.l %d0-%d3,-(%sp)
    clr.l   %d0
    clr.l   %d1
    clr.l   %d2
    clr.l   %d3
    movem.l (%sp)+,%d0-%d3
    add.l   %d1,%d0
    add.l   %d2,%d0
    add.l   %d3,%d0
    movem.l (%sp)+,%d1-%d3          | restore the outer three, keep the sum
    bsr     record

    movem.w one_two,%d0-%d1         | word load sign-extends into the long
    move.l  %d1,%d0
    bsr     record

| --- address registers are 32-bit and set no flags -------------------------
    move.w  #0xffff,%d0
    add.w   #1,%d0                  | Z, C, X set
    movea.w #-2,%a0                 | sign-extends, no flags
    addq.w  #1,%a0                  | full-width add, no flags
    move.l  %a0,%d0
    bsr     record

    lea     8(%a6),%a1
    lea     -8(%a1),%a2
    move.l  %a1,%d0
    sub.l   %a2,%d0                | lea arithmetic, 32 bits wide
    bsr     record

    stop    #0x2700

| ---------------------------------------------------------------------------
| Store one result: the value, then the condition codes as they stood before
| this routine touched anything.
record:
    move.w  %sr,%d7
    move.l  %d0,(%a6)+
    move.w  %d7,(%a6)+
    rts

    .align 2
one_two:
    .word   0xfffe
    .word   0x0002
byte_scratch:
    .byte   0
    .align 2
results:
    .space  512
