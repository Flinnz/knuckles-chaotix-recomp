! SH-2 semantics test. Each case computes something whose correct answer is
! known independently of any SH-2 model, so a wrong result points at the
! recompiler rather than at the expected value.
!
! Results are written as longs to the block at 0x06008000.

	.section .text
	.align 2
start:
	! This routine makes a call, so it must preserve PR itself: on SH-2 a
	! call only writes PR, and rts jumps to whatever PR holds. Without this
	! the final rts would return to the address the jsr below left there.
	sts.l	pr,@-r15
	mov.l	res_base,r14

	! -- 1. unsigned divide, 1000 / 7 -> 142 remainder 6 --------------
	! The SH-2 has no divide instruction: this is the manual's 32-step
	! restoring division, which exercises div1's Q/M/T bookkeeping fully.
	mov.l	dividend,r3
	mov.l	divisor,r2
	mov	#0,r1
	div0u
	.rept 32
	rotcl	r3
	div1	r2,r1
	.endr
	rotcl	r3
	mov.l	r3,@r14
	! div1 is non-restoring: the last step may have over-subtracted, so the
	! remainder needs the divisor added back. Real division routines do this.
	cmp/pz	r1
	bt	remok
	add	r2,r1
remok:
	mov.l	r1,@(4,r14)

	! -- 2. a delay slot runs even when its branch is not taken -------
	! bf/s executes the delay slot on every iteration including the last,
	! so r5 counts 5, not 4.
	mov	#5,r0
	mov	#0,r5
lp:
	dt	r0
	bf/s	lp
	add	#1,r5
	mov.l	r5,@(8,r14)

	! -- 3. a delay slot clobbering the register its branch jumps through
	! The target is latched when jsr executes, so overwriting r1 in the
	! delay slot must not change where control goes.
	mov.l	tgt,r1
	mov	#0,r6
	jsr	@r1
	mov	#99,r1
	mov.l	r6,@(12,r14)

	! -- 4. carry propagation through a 64-bit add --------------------
	! 0x1FFFFFFFF + 0x300000002 = 0x500000001
	mov.l	loA,r7
	mov.l	hiA,r8
	mov.l	loB,r9
	mov.l	hiB,r10
	clrt
	addc	r9,r7
	addc	r10,r8
	mov.l	r7,@(16,r14)
	mov.l	r8,@(20,r14)

	! -- 5. arithmetic vs logical shift right -------------------------
	mov.l	neg8,r11
	mov	r11,r12
	shar	r12
	mov.l	r12,@(24,r14)
	mov	r11,r13
	shlr	r13
	mov.l	r13,@(28,r14)

	lds.l	@r15+,pr
	rts
	nop

	.align 2
sub7:
	mov	#7,r6
	rts
	nop

	.align 2
res_base:	.long 0x06008000
dividend:	.long 1000
divisor:	.long 7
tgt:		.long sub7
loA:		.long 0xFFFFFFFF
hiA:		.long 0x00000001
loB:		.long 0x00000002
hiB:		.long 0x00000003
neg8:		.long 0xFFFFFFF8
