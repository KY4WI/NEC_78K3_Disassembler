; Synthetic firmware-style image for the CALLT test: base 8000H, a CALLT
; table at 8040H (two slots share a routine, one points at erased ROM),
; and code that uses it. Disassemble with --callt 0x8040.
	.org	0x8000
	.space	0x40, 0xff
	.word	r_a, r_b, r_a, r_c
	.space	0x8080-$, 0xff
start:
	callt	[0]
	callt	[1]
	callt	[2]
	callt	[3]
	callt	[5]
	br	start
r_a:	ret
r_b:	nop
	ret
r_c:	mov	a, #1
	ret
	.space	0x8100-$, 0xff
