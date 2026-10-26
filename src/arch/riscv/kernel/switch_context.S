.section ".text"
	.global switch_context
switch_context:
	sd ra, 0(a0)
	sd sp, 8(a0)
	sd s0, 16(a0)
	sd s1, 24(a0)
	sd s2, 32(a0)
	sd s3, 40(a0)
	sd s4, 48(a0)
	sd s5, 56(a0)
	sd s6, 64(a0)
	sd s7, 72(a0)
	sd s8, 80(a0)
	sd s9, 88(a0)
	sd s10, 96(a0)
	sd s11, 104(a0)

	ld ra, 0(a1)
	ld sp, 8(a1)
	ld s0, 16(a1)
	ld s1, 24(a1)
	ld s2, 32(a1)
	ld s3, 40(a1)
	ld s4, 48(a1)
	ld s5, 56(a1)
	ld s6, 64(a1)
	ld s7, 72(a1)
	ld s8, 80(a1)
	ld s9, 88(a1)
	ld s10, 96(a1)
	ld s11, 104(a1)

	ret

.section ".text"
/* void __switch_to(unsigned long *old_sp, unsigned long *new_sp) */
	.global __switch_to
__switch_to:
	addi t0, sp, -112
	sd t0, 0(a0)

	sd s0, 0(t0)
	sd s1, 8(t0)
	sd s2, 16(t0)
	sd s3, 24(t0)
	sd s4, 32(t0)
	sd s5, 40(t0)
	sd s6, 48(t0)
	sd s7, 56(t0)
	sd s8, 64(t0)
	sd s9, 72(t0)
	sd s10, 80(t0)
	sd s11, 88(t0)
	sd ra, 96(t0)

	ld t0, 0(a1)
	addi sp, t0, 112

	ld s0, 0(t0)
	ld s1, 8(t0)
	ld s2, 16(t0)
	ld s3, 24(t0)
	ld s4, 32(t0)
	ld s5, 40(t0)
	ld s6, 48(t0)
	ld s7, 56(t0)
	ld s8, 64(t0)
	ld s9, 72(t0)
	ld s10, 80(t0)
	ld s11, 88(t0)
	ld ra, 96(t0)

	ret
