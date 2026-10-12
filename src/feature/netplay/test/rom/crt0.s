@ linktest.gba startup code.
@ SPDX-License-Identifier: MIT
@
@ Minimal GBA startup: cartridge header, stacks, .bss clear, IRQ vector,
@ then a Thumb call into main(). No BIOS calls are needed to boot, so the
@ ROM runs the same under the HLE BIOS and a real one.

	.section .crt0, "ax"
	.arm
	.global _start
_start:
	b	_reset

	@ Nintendo logo area (unchecked by the HLE BIOS), 156 bytes.
	.fill	156, 1, 0
	@ Title (12), game code (4), maker code (2)
	.ascii	"MGBANETPLAY "
	.ascii	"NPLT"
	.ascii	"01"
	.byte	0x96		@ fixed value
	.byte	0x00		@ main unit code
	.byte	0x00		@ device type
	.fill	7, 1, 0		@ reserved
	.byte	0x00		@ software version
	.byte	0x00		@ complement check, patched by the Makefile
	.fill	2, 1, 0		@ reserved

_reset:
	@ IRQ mode stack
	mov	r0, #0x12
	msr	cpsr_c, r0
	ldr	sp, =0x03007FA0
	@ System mode stack
	mov	r0, #0x1F
	msr	cpsr_c, r0
	ldr	sp, =0x03007F00

	@ Clear .bss
	ldr	r0, =__bss_start
	ldr	r1, =__bss_end
	mov	r2, #0
1:	cmp	r0, r1
	strlo	r2, [r0], #4
	blo	1b

	@ Install the IRQ handler the BIOS dispatches to
	ldr	r0, =0x03007FFC
	ldr	r1, =irq_handler
	str	r1, [r0]

	ldr	r0, =main
	mov	lr, pc
	bx	r0
2:	b	2b

	.pool
