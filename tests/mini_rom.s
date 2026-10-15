; Synthetic 8 KB "mask ROM" for the --source round-trip test. Shaped like
; a real 78K/III mask ROM (vector table, handlers, a string, a table, a
; CALLF target, erased 0xFF filler, a fixed-address entry table at 1F00H)
; but entirely made up -- no vendor code.
	.org	0x0000
	.word	reset, int_x, int_x, 0xffff, 0xffff, 0xffff, int_x, timer0
	.space	0x40-$, 0xff
reset:
	movw	sfr[0xfc].w, #0xfe00
	call	!init
	callf	!farsub
idle:	br	idle
init:
	mov	a, #0x12
	mov	0xfe28, a
	movw	hl, #msg
	movw	ax, !table
	bt	0xfe28, skip, 3
	set1	0xfe28, 4
	mov	a, [table+de]
skip:	ret
timer0:
	push	{ax,bc}
	inc	0xfe30
	pop	{ax,bc}
int_x:	reti
msg:	.ascii	"TEST ROM\x00"
table:	.word	0x1234, 0x5678
	.space	0x0800-$, 0xff
	.org	0x0800
farsub:	dec	0xfe31
	ret
	.space	0x1f00-$, 0xff
	.org	0x1f00
	br	!reset
	br	!init
	br	!farsub
	.space	0x2000-$, 0xff
