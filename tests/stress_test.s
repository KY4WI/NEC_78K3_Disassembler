; =====================================================================
; tests/stress_test.s -- disassembler stress test (source)
;
; A sequence of paired test cases. Each pair is a real instruction,
; assembled by vasm's 78K/III backend, followed by a hand-derived .byte
; sequence encoding the same instruction from the manual. Disassembling
; the result must print each pair as two consecutive, identical lines;
; tests/run_stress_test.sh (make test) checks that for every pair.
;
; stress_test.bin is checked in, built from this file with the vasm
; 78K/III backend (github.com/KY4WI/vasm). After changing this file,
; assemble it again and replace stress_test.bin; vasm's std syntax still
; accepts this file's older operand forms (set1 x, 3 / bt x, label, 3 /
; sfr[x].w).
;
; What it catches: a pair can only differ if the assembler's bytes
; disagree with the hand-derived bytes, so it validates the assembler
; against the manual, and checks that the disassembler prints each
; encoding as the expected text. A decoding error that prints the same
; wrong text for both halves still passes; the printed text was checked
; against each case's cited manual page by eye for that reason. Cases
; 23-64 (conditional branches, BT/BF/BTCLR/BFSET, DBNZ) were verified by
; hand against the uPD78312A User's Manual, chapter 10.3, "OP Code of
; Each Instruction" (pp. 10-57 to 10-60).
;
; Notes in the cases below cite the vasm backend's copies of the tables
; (shared/Opcodes.h, shared/RegisterNames.h, shared/Addressing.h under
; cpus/78k3/); this repository's Opcodes.h is identical to that
; Opcodes.h apart from the mnemonic prefix and comments, and Decode.h is
; its decoder. Coverage grows family by family; add new cases at the end
; in the same numbered, cited style.
; =====================================================================

	.section .text

; ---- 1. MOV A,r1 -- register addressing, 3-bit r1 field -----------
; Manual (p.63, 4.2.1 Example 1): "Operation code 1 1 0 1 0 R2 R1 R0";
; "To select the R2 register as r1, describe: MOV A, R2" (R2's function
; name is C) -> opcode 1101_0010 = 0xD2.
	mov	a, c
	.byte	0xd2

; ---- 2. INCW rp2 -- register addressing, 2-bit S1S0 field ----------
; Manual (p.63, 4.2.1 Example 2): "Operation code 0 1 0 0 0 1 S1 S0";
; "To select DE as rp2, describe: INCW DE" -> opcode 0100_0110 = 0x46.
	incw	de
	.byte	0x46

; ---- 3. ADD A,#byte -- immediate addressing -------------------------
; Manual (p.64, 4.2.2 Example): "Operation code 1 0 1 0 1 0 0 0" then
; the data byte; "To take 77H as byte: ADD A, #77H" -> 0xA8, 0x77.
	add	a, #0x77
	.byte	0xa8, 0x77

; ---- 4. MOV saddr,saddr -- two-operand short-direct ----------------
; The second byte is the SOURCE and the third the DESTINATION, both
; plain saddr offsets. Real code settles this: mask ROM 179BH (38 26 2C)
; copies panel_rx_byte FE26H to FE2CH, and TK-941 AB00 C17FH (38 F0 29)
; loads eeprom_addr FE29H from X inside a 16-byte EEPROM write loop.
; (The manual's Figure 4-6 had been read as "MOV 0FE20H, 0FE50H ->
; 38 20 60" with a bit-5/4 swap; real code contradicts both the order
; and the swap. The same three bytes mean MOV 0FE60H, 0FE20H.)
	mov	0xfe60, 0xfe20
	.byte	0x38, 0x20, 0x60

; ---- 5. MOV sfr,A -- special function register addressing ----------
; Manual (p.67, 4.2.5 Example): "Operation code 0 0 0 1 0 0 1 0" then
; the sfr-offset byte; "To specify PM0 as sfr: MOV PM0, A" -> PM0's
; sfr-offset is 0x20 (effective address FF20H) -> 0x12, 0x20.
	mov	sfr[0x20], a
	.byte	0x12, 0x20

; ---- 6. XCH A,[saddrp] -- short direct memory indirect --------------
; Manual (p.68, 4.2.6 Example): "Operation code 0 0 1 0 0 0 1 1" then
; the saddr-offset byte; "To take FEA0H as saddrp: XCH A, [0FEA0H]" ->
; 0x23, 0xA0 (FEA0H - FE00H = 0xA0, the plain saddr formula).
	xch	a, [0xfea0]
	.byte	0x23, 0xa0

; ---- 7. MOV A,mem -- register-indirect short form, plain -----------
; Manual (p.69, 4.2.7 Example 1, first bullet): "0 1 0 1 1 mem" (1
; byte, MOV's own shortcut encoding for [DE]/[HL]/[DE+]/[HL+]/[DE-]/
; [HL-]); "To specify [DE] as mem: MOV A, [DE]" -> opcode 0101_1100 =
; 0x5C (mem code for plain [DE] = 4, confirmed against
; shared/RegisterNames.h's REGNAMES_MEM_S table: 01011_100 = 0x5C).
	mov	a, [de]
	.byte	0x5c

; ---- 8. MOV A,mem -- register-indirect short form, increment -------
; Same 1-byte "01011 mem" family as #7, mem code for [HL+] = 1 (per
; REGNAMES_MEM_S) -> 01011_001 = 0x59.
	mov	a, [hl+]
	.byte	0x59

; ---- 9. ROR4 rp1 -- bare rp1/Q-code field (register-indirect op) ----
; Manual (p.69, 4.2.7 Example 2): "Operation code 0 0 0 0 0 1 0 1" then
; "1 0 0 0 1 Q2 Q1 Q0"; "To select RP0 as rp1: ROR4 [RP0]" (RP0's
; function name is AX) -> 0x05, 0x88 (Q-code for RP0/AX = 0). The
; manual writes the operand with brackets (ROR4 operates on the memory
; [rp1] points to), but shared/Opcodes.h encodes this row as a bare
; OT_RP1 field (not the bracketed OT_RP1P used elsewhere), and this
; codebase's assembler/disassembler both follow the table, not the
; manual's notation -- so the real text is unbracketed "ror4 ax".
	ror4	ax
	.byte	0x05, 0x88

; ---- 10. AND A,mem -- based addressing ------------------------------
; Manual (p.70, 4.2.8 Example): "Operation code 0 0 0 0 0 1 1 0" then
; "0 mem 1 1 0 0" then an offset byte; "To select based addressing of
; VP + 10H: AND A, [VP+10H]" -> 0x06, 0x4C, 0x10 (mem code for VP = 4).
	and	a, [vp+0x10]
	.byte	0x06, 0x4c, 0x10

; ---- 11. MULUW rp1 -- bare (non-bracketed) rp1/Q-code field --------
; shared/Opcodes.h: { M_MULUW, opcode 0x05, 0b00101000 | rp1 }. DE's
; Q-code is 5 (shared/RegisterNames.h REGNAMES_RP1) -> byte1 =
; 0x28 | 5 = 0x2D. Checks the Q-code -> "de" register text.
	muluw	de
	.byte	0x05, 0x2d

; ---- 12. DIVUX rp1 -- bare rp1/Q-code field, tick-mark case --------
; shared/Opcodes.h: { M_DIVUX, opcode 0x05, 0b11101000 | rp1 }. AX'
; (RSS=1 bank)'s Q-code is 4 -> byte1 = 0xE8 | 4 = 0xEC. Exercises
; Decode.h's "print a trailing tick for Q-code 4 or 6" rule.
	divux	ax'
	.byte	0x05, 0xec

; ---- 13. PUSH {..} -- post-byte register list -----------------------
; Manual (p.94, Figure 5-1): bit0=RP0(AX) bit1=RP1(BC) bit2=RP2(AX')
; bit3=RP3(BC') bit4=RP4(VP) bit5=RP5(UP) bit6=RP6(DE) bit7=RP7(HL).
; {VP,UP} -> bits 4 and 5 set -> 0x30. Deliberately NOT {ax,bc,de,hl}:
; that combination's bit set is symmetric under a full bit-reversal
; (ax/hl and bc/de mirror each other), so it can't tell a correct
; post-byte mapping from a mirrored one. {vp,up} has no such symmetry.
	push	{vp, up}
	.byte	0x35, 0x30

; ---- 14. PUSHU {..} -- post-byte_U, PSW-vs-UP distinction ----------
; Same bit positions as #13 (Figure 5-1's Note: "RP5 (UP) is set for
; the PUSH and POP instructions and PSW is set for the PUSHU and POPU
; instructions") -- {VP,PSW} uses the same bits (4,5) as {vp,up} above
; but through PUSHU's own opcode (0x37) and name table, to independently
; exercise the OT_POST_BYTE_U "i==5 means psw, not up" special case.
	pushu	{vp, psw}
	.byte	0x37, 0x30

; ---- 15. CALLT [addr5] -- table indirect -----------------------------
; Manual (p.61, Figure 4-3): "Operation code 1 1 1 ta" (1 byte, ta is
; the low 5 bits, used here as a raw 0-0x1F index, matching this
; project's own OT_ADDR5 print format "[0x00xx]" -- see
; Decode.h). ta=5 -> 111_00101 = 0xE5.
	callt	[0x05]
	.byte	0xe5

; ---- 16. CALLF !addr11 -----------------------------------------------
; shared/Opcodes.h: { M_CALLF, opcode base 0b10010000, mask 0b11111000
; -- top 5 bits fixed, low 3 bits of byte0 = f10..f8, byte1 = f7..f0,
; target = 0x0800 + the 11-bit field } (Decode.h's OT_ADDR11
; comment). Target 0x0a3c -> raw11 = 0x023c -> byte0 = 0x90 | 0x02 =
; 0x92, byte1 = 0x3c.
	callf	0x0a3c
	.byte	0x92, 0x3c

; ---- 17. BR $addr16r -- PC-relative branch ---------------------------
; shared/Opcodes.h: opcode 0x14, OT_ADDR16R (1 byte signed displacement,
; target = next-instruction's address + disp -- confirmed against real
; silicon: the TK-x4x mask ROM's own self-loop idiom disassembles as
; "14 fe  br  <own address>", i.e. disp=-2 from the address right after
; a 2-byte branch). target_br1 is placed immediately after the raw
; copy below, so the raw instruction's own disp is 0 by definition
; (target IS its own next-PC) -- no address arithmetic needed, and
; this doesn't depend on shared/Opcodes.h's bit math at all. vasm
; computes the real instruction's disp (2, since it's 2 bytes before
; the same target) on its own.
	br	target_br1
	.byte	0x14, 0x00
target_br1:

; ---- 18. INC r1 -- register direct ------------------------------------
; shared/Opcodes.h: { M_INC, opcode base 0b11000000, mask low 3 bits }.
; C's r1 code is 2 (shared/RegisterNames.h REGNAMES_R1) -> 0xC0|2=0xC2.
	inc	c
	.byte	0xc2

; ---- 19. DEC r1 -- register direct ------------------------------------
; shared/Opcodes.h: { M_DEC, opcode base 0b11001000, mask low 3 bits }.
; B's r1 code is 3 -> 0xC8|3=0xCB.
	dec	b
	.byte	0xcb

; ---- 20. SET1 CY -- single fixed byte, no operand ---------------------
; shared/Opcodes.h: { M_SET1, 1 byte, 0b01000001 } -> 0x41.
	set1	cy
	.byte	0x41

; ---- 21. CLR1 CY -- single fixed byte, no operand ---------------------
; shared/Opcodes.h: { M_CLR1, 1 byte, 0b01000000 } -> 0x40.
	clr1	cy
	.byte	0x40

; ---- 22. SET1 A,bit -- fixed-register operand plus a 3-bit field -----
; shared/Opcodes.h: { M_SET1, byte0 fixed 0x03, byte1 base 0b10001000
; | bit }. bit=5 -> byte1 = 0x88|5 = 0x8D.
	set1	a, 5
	.byte	0x03, 0x8d

; ---- 23-36. Bcc $addr16r -- conditional branches ---------------------
; One-byte-opcode family (BL/BNL/BE/BNE/BPE/BPO/BN/BP) and two-byte-
; opcode family (BGT/BGE/BLT/BLE/BH/BNH). All share the exact same
; OT_ADDR16R relative-branch operand already verified once via BR (#17
; above); what's actually being checked here is that each condition
; code maps to its own distinct, correct opcode byte(s) in
; shared/Opcodes.h, not a mixed-up or copy-pasted one -- these 14 rows
; sit right next to each other in the table and have never individually
; been exercised before. Same self-referencing-target technique as #17:
; each real instruction branches to a label placed immediately after its
; own hand-derived raw bytes, so vasm computes that real instruction's
; displacement on its own (always a small positive number -- exactly the
; length of its own encoding, since the target sits one encoding-length
; past the raw bytes), and the raw half's last byte is always 0x00
; (disp=0, "target = my own next PC") for the identical reason #17 used
; 0x00. Opcode bytes verified against the NEC uPD78312A User's Manual
; (Chapter 10.3 "OP Code of Each Instruction," p.10-57/10-58,
; "Conditional branch instruction" OP Code table -- read directly off
; the manual's own bit-pattern columns, not off shared/Opcodes.h): the
; one-byte family is a flat 1000_0ccc condition-code field (BNE=0x80,
; BE=0x81, BPO=0x84, BPE=0x85, BNL=0x82, BL=0x83, BP=0x86, BN=0x87);
; the two-byte family is fixed byte0=0x07 followed by a second opcode
; byte whose low nibble carries the condition code (BLT=0xF8, BGE=0xF9,
; BLE=0xFA, BGT=0xFB, BNH=0xFC, BH=0xFD). All 14 match shared/Opcodes.h
; exactly -- no discrepancy found.
	bnl	target_b1
	.byte	0x82, 0x00
target_b1:
	bl	target_b2
	.byte	0x83, 0x00
target_b2:
	be	target_b3
	.byte	0x81, 0x00
target_b3:
	bne	target_b4
	.byte	0x80, 0x00
target_b4:
	bpe	target_b5
	.byte	0x85, 0x00
target_b5:
	bpo	target_b6
	.byte	0x84, 0x00
target_b6:
	bn	target_b7
	.byte	0x87, 0x00
target_b7:
	bp	target_b8
	.byte	0x86, 0x00
target_b8:
	bgt	target_b9
	.byte	0x07, 0xfb, 0x00
target_b9:
	bge	target_b10
	.byte	0x07, 0xf9, 0x00
target_b10:
	blt	target_b11
	.byte	0x07, 0xf8, 0x00
target_b11:
	ble	target_b12
	.byte	0x07, 0xfa, 0x00
target_b12:
	bh	target_b13
	.byte	0x07, 0xfd, 0x00
target_b13:
	bnh	target_b14
	.byte	0x07, 0xfc, 0x00
target_b14:

; ---- 37-62. BT/BF/BTCLR/BFSET saddr/sfr/a/x/pswh/pswl,bit,$addr16r ----
; Bit-test-and-branch family: BT branches if the bit is 1, BF if it's 0,
; BTCLR tests-then-clears-if-set, BFSET tests-then-sets-if-clear. Same
; shape for all four: a 3-bit field (bit 0-7, via OT_N_BIT) packed into
; either byte0's low 3 bits (the saddr form only) or byte1's low 3 bits
; (every other form), a base-operand selector (saddr address / sfr
; offset / fixed "a"/"x"/"pswh"/"pswl" register), then the branch
; displacement as the last byte -- so this reuses case 17's self-
; referencing-target technique exactly like cases 23-36 above: every
; raw half's last byte is 0x00, and the label placed right after it is
; both halves' real target, with vasm computing the real instruction's
; own displacement on its own.
;
; Bytes verified against the NEC uPD78312A User's Manual (Chapter 10.3
; "OP Code of Each Instruction," p.10-58/10-59/10-60, the BT/BF/BTCLR/
; BFSET OP Code table rows) for each of these 24 rows (6 base-operand
; forms x 4 mnemonics), read directly off the manual's own bit-pattern
; columns rather than from the opcode table, so an error shared by the
; assembler's and disassembler's copies of the table would show up. All
; 24 match, including one asymmetry the manual confirms is real: BT's saddr form is a compact 3-byte
; encoding with the bit number packed into byte0 (opcode 0x70|bit, no
; 0x08 prefix), while BF/BTCLR/BFSET's saddr forms -- and every mnemonic's
; sfr/a/x/pswh/pswl forms, BT included -- use the longer 0x08-prefixed
; 4-byte (or 3-byte for a/x/pswh/pswl) encoding. The two
; "0x03 vs 0x02" and "0x.8 vs 0x.0" bucket boundaries below are not a
; typo -- "a" and "pswh" share one subOp bucket, "x" and "pswl" share
; the other, and byte0 (0x03 vs 0x02) is what actually distinguishes
; the A/X pair from the PSWH/PSWL pair.
;
; BT (op base 0x70|bit for saddr; 0x08/0x03/0x02 + subOp|bit otherwise):
	bt	0xfe30, target_b15, 3
	.byte	0x73, 0x30, 0x00
target_b15:
	bt	sfr[0x10], target_b16, 3
	.byte	0x08, 0xbb, 0x10, 0x00
target_b16:
	bt	a, target_b17, 3
	.byte	0x03, 0xbb, 0x00
target_b17:
	bt	x, target_b18, 3
	.byte	0x03, 0xb3, 0x00
target_b18:
	bt	pswh, target_b19, 3
	.byte	0x02, 0xbb, 0x00
target_b19:
	bt	pswl, target_b20, 3
	.byte	0x02, 0xb3, 0x00
target_b20:

; BF (same shape as BT, subOp bucket 0xa0/0xa8 instead of 0xb0/0xb8):
	bf	0xfe30, target_b21, 3
	.byte	0x08, 0xa3, 0x30, 0x00
target_b21:
	bf	sfr[0x10], target_b22, 3
	.byte	0x08, 0xab, 0x10, 0x00
target_b22:
	bf	a, target_b23, 3
	.byte	0x03, 0xab, 0x00
target_b23:
	bf	x, target_b24, 3
	.byte	0x03, 0xa3, 0x00
target_b24:
	bf	pswh, target_b25, 3
	.byte	0x02, 0xab, 0x00
target_b25:
	bf	pswl, target_b26, 3
	.byte	0x02, 0xa3, 0x00
target_b26:

; BTCLR (subOp bucket 0xd0/0xd8):
	btclr	0xfe30, target_b27, 3
	.byte	0x08, 0xd3, 0x30, 0x00
target_b27:
	btclr	sfr[0x10], target_b28, 3
	.byte	0x08, 0xdb, 0x10, 0x00
target_b28:
	btclr	a, target_b29, 3
	.byte	0x03, 0xdb, 0x00
target_b29:
	btclr	x, target_b30, 3
	.byte	0x03, 0xd3, 0x00
target_b30:
	btclr	pswh, target_b31, 3
	.byte	0x02, 0xdb, 0x00
target_b31:
	btclr	pswl, target_b32, 3
	.byte	0x02, 0xd3, 0x00
target_b32:

; BFSET (subOp bucket 0xc0/0xc8):
	bfset	0xfe30, target_b33, 3
	.byte	0x08, 0xc3, 0x30, 0x00
target_b33:
	bfset	sfr[0x10], target_b34, 3
	.byte	0x08, 0xcb, 0x10, 0x00
target_b34:
	bfset	a, target_b35, 3
	.byte	0x03, 0xcb, 0x00
target_b35:
	bfset	x, target_b36, 3
	.byte	0x03, 0xc3, 0x00
target_b36:
	bfset	pswh, target_b37, 3
	.byte	0x02, 0xcb, 0x00
target_b37:
	bfset	pswl, target_b38, 3
	.byte	0x02, 0xc3, 0x00
target_b38:

; ---- 63-64. DBNZ r2/saddr,$addr16r -- decrement-and-branch-if-nonzero -
; Verified against the NEC uPD78312A User's Manual (Chapter 10.3, p.10-60,
; same OP Code table page as BFSET's last rows): register form is a flat
; opcode 0011001_C0 (C0=0 selects C, C0=1 selects B) with the branch
; displacement as its only other byte -- matches shared/Opcodes.h's
; 0b00110010/mask 0b11111110 exactly; saddr form is a fixed opcode byte
; 0x3b (0011_1011) then the saddr offset then the displacement -- also
; an exact match. Same self-referencing-target technique again. "c"'s
; r2-select bit is 0.
	dbnz	c, target_b39
	.byte	0x32, 0x00
target_b39:
	dbnz	0xfe30, target_b40
	.byte	0x3b, 0x30, 0x00
target_b40:

; =====================================================================
; GROUP: 8-bit Data Transfer Instructions (MOV, XCH) -- cases 65-93.
;
; Starting here, new cases are being added by walking Chapter 10.3, "OP
; Code of Each Instruction," of the actual NEC manufacturer manual IN
; TABLE ORDER, one instruction-group page-run at a time, rather than
; picking instructions ad hoc. Source document for this and every
; group below, unless a case says otherwise:
;
;   NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
;   16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of
;   Each Instruction." Bitsavers copy:
;   https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; This group's rows are on pages 10-42 and 10-43 (MOV, then XCH). Every
; byte below was read directly off those two pages' OP Code columns,
; not off shared/Opcodes.h. Register field VALUES (which bit pattern
; means "c" vs "l" vs "pswh" etc.) come from shared/RegisterNames.h's
; REGNAMES_R1/REGNAMES_R tables, not from the manual's own register
; figures -- that text<->code mapping isn't what's in question here
; (it's exercised, correctly, by every pair in this whole file); it's
; each ROW's bit LAYOUT that's being checked against the real source.
; One case per table row that case #1-22 didn't already cover (a
; couple of rows -- A,mem*'s register-indirect shortcut, and XCH
; A,[saddrp] -- already have a case each from the original 22 and
; aren't repeated here).
;
; Extended (mod/mem) addressing cases below use Base Mode (mod field
; 0b00110), as case #10 does.
; =====================================================================

; ---- 65. MOV r1,#byte -- p.10-42 ----
; B1 = 1011_1_R2R1R0 (r1="x", REGNAMES_R1 code 0); B2 = data. (Not
; "b'": this assembler's parser rejects a tick-mark r1 register as the
; first operand of this specific {r1,#byte} syntax class -- confirmed
; in isolation, "mov b', #imm" fails to assemble while "mov l, b'"
; (the r,r1 row below) assembles fine, so it's a narrow parser gap in
; this one operand pairing, not a tick-name problem generally. Picking
; a non-tick r1 register sidesteps it without losing row coverage.)
	mov	x, #0x42
	.byte	0xb8, 0x42

; ---- 66. MOV saddr,#byte -- p.10-42 ----
; B1 = 0011_1010 = 0x3A; B2 = saddr-offset; B3 = data.
	mov	0xfe55, #0x99
	.byte	0x3a, 0x55, 0x99

; ---- 67. MOV sfr,#byte -- p.10-42 ----
; B1 = 0010_1011 = 0x2B; B2 = sfr-offset; B3 = data.
	mov	sfr[0x22], #0x07
	.byte	0x2b, 0x22, 0x07

; ---- 68. MOV r,r1 -- p.10-42 ----
; B1 = 0010_0100 = 0x24; B2 = R3R2R1R0(dest,4-bit) 0 R2R1R0(src,3-bit).
; dest="l" (REGNAMES_R code 14 = 1110), src="b'" (REGNAMES_R1 code 7
; = 111) -> B2 = 1110_0_111 = 0xE7.
	mov	l, b'
	.byte	0x24, 0xe7

; ---- 69. MOV A,saddr -- p.10-42 ----
; B1 = 0010_0000 = 0x20; B2 = saddr-offset.
	mov	a, 0xfe66
	.byte	0x20, 0x66

; ---- 70. MOV saddr,A -- p.10-42 ----
; B1 = 0010_0010 = 0x22; B2 = saddr-offset.
	mov	0xfe77, a
	.byte	0x22, 0x77

; ---- 71. MOV A,sfr -- p.10-42 ----
; B1 = 0001_0000 = 0x10; B2 = sfr-offset.
	mov	a, sfr[0x24]
	.byte	0x10, 0x24

; ---- 72-73. MOV A,mem* -- p.10-42, two more of the six 1-byte
; register-indirect mem codes (mem=4 [de] and mem=1 [hl+] were already
; covered by cases 7-8; this adds mem=0 [de+] and mem=5 [hl]). B1 =
; 0101_1_mem.
	mov	a, [de+]
	.byte	0x58
	mov	a, [hl]
	.byte	0x5d

; ---- 74-75. MOV mem,A* -- p.10-42, reverse direction of 72-73, not
; otherwise covered (mem=2 [de-], mem=3 [hl-]). B1 = 0101_0_mem.
	mov	[de-], a
	.byte	0x52
	mov	[hl-], a
	.byte	0x53

; ---- 76. MOV A,[saddrp] -- p.10-43 ----
; B1 = 0001_1000 = 0x18; B2 = saddr-offset. saddrp must be even.
	mov	a, [0xfe80]
	.byte	0x18, 0x80

; ---- 77. MOV [saddrp],A -- p.10-43 ----
; B1 = 0001_1001 = 0x19; B2 = saddr-offset.
	mov	[0xfe82], a
	.byte	0x19, 0x82

; ---- 78. MOV A,!addr16 -- p.10-43 ----
; B1 = 0000_1001 = 0x09; B2 = 1111_0000 = 0xF0; B3/B4 = low/high addr.
; The '!' marks the absolute form; it must not be read as a logical NOT
; (which would turn the address into 0).
	mov	a, !0x1234
	.byte	0x09, 0xf0, 0x34, 0x12

; ---- 79. MOV !addr16,A -- p.10-43 ----
; B1 = 0x09; B2 = 1111_0001 = 0xF1; B3/B4 = low/high addr. As case 78,
; with the operand in the other slot.
	mov	!0x5678, a
	.byte	0x09, 0xf1, 0x78, 0x56

; ---- 80-81. MOV PSWL/PSWH,#byte -- p.10-43 ----
; Same B1 as the general sfr,#byte row (0x2B), but with the sfr-offset
; byte FIXED (0xFE for PSWL, 0xFF for PSWH) instead of a variable
; offset -- PSWL/PSWH are addressed as fixed pseudo-sfr slots, not a
; parsed sfr operand. B3 = data in both cases.
	mov	pswl, #0x03
	.byte	0x2b, 0xfe, 0x03
	mov	pswh, #0x80
	.byte	0x2b, 0xff, 0x80

; ---- 82-83. MOV PSWL/PSWH,A -- p.10-43 ----
; B1 = 0001_0010 = 0x12 (same fixed-offset pattern as above).
	mov	pswl, a
	.byte	0x12, 0xfe
	mov	pswh, a
	.byte	0x12, 0xff

; ---- 84-85. MOV A,PSWL/PSWH -- p.10-43 ----
; B1 = 0001_0000 = 0x10 (same fixed-offset pattern again).
	mov	a, pswl
	.byte	0x10, 0xfe
	mov	a, pswh
	.byte	0x10, 0xff

; ---- 86. MOV A,mem (extended, Base Mode) -- p.10-42 ----
; B1 = 000_mod = 0b000_00110 = 0x06 (mod=0b00110, Base Mode, p.10-41).
; B2 = 0(dir) mem(3-bit, Base Mode column: 010=[HL+byte]) 0000 = 0x20.
; B3 = low offset (Base Mode has no high-offset byte -- confirmed by
; this file's original case #10, the only other Base Mode case here).
	mov	a, [hl+0x08]
	.byte	0x06, 0x20, 0x08

; ---- 87. MOV mem,A (extended, Base Mode) -- p.10-42 ----
; Same B1; B2 = 1(dir) mem(100=[VP+byte]) 0000 = 0xC0.
	mov	[vp+0x15], a
	.byte	0x06, 0xc0, 0x15

; ---- 88. XCH A,r1 -- p.10-43 ----
; B1 = 1101_1_R2R1R0 (r1="x'", code 4) = 0xDC.
	xch	a, x'
	.byte	0xdc

; ---- 89. XCH r,r1 -- p.10-43 ----
; B1 = 0010_0101 = 0x25; B2 = dest(4-bit)=d(13) 0 src(3-bit)=c'(6)
; -> 1101_0_110 = 0xD6.
	xch	d, c'
	.byte	0x25, 0xd6

; ---- 90. XCH A,mem (extended, Base Mode) -- p.10-43 ----
; B1 = 0x06; B2 = 0(dir) mem(001=[SP+byte]) 0100 = 0x14 (XCH's own
; trailing nibble, 0100, differs from MOV's 0000 -- each mnemonic has
; its own signature nibble in this shared field, confirmed by reading
; both rows directly rather than assuming they match).
	xch	a, [sp+0x30]
	.byte	0x06, 0x14, 0x30

; ---- 91. XCH A,saddr -- p.10-43 ----
; B1 = 0010_0001 = 0x21; B2 = saddr-offset.
	xch	a, 0xfe90
	.byte	0x21, 0x90

; ---- 92. XCH A,sfr -- p.10-43 ----
; B1 = 0000_0001 = 0x01; B2 = 0010_0001 = 0x21; B3 = sfr-offset.
	xch	a, sfr[0x26]
	.byte	0x01, 0x21, 0x26

; ---- 93. XCH saddr,saddr -- p.10-43 ----
; B1 = 0011_1001 = 0x39; B2/B3 = source/dest saddr-offset.
	xch	0xfea2, 0xfeb4
	.byte	0x39, 0xb4, 0xa2

; =====================================================================
; GROUP: 16-bit Data Transfer Instructions (MOVW, XCHW) -- cases 94-106.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction."
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; The entire MOVW/XCHW table fits on a single page, p.10-44 (PDF page
; 291 of the bitsavers scan) -- unlike the 8-bit Data Transfer group
; above, it isn't split across pages, so every case below cites that
; one page. Register-pair field notes: "rp" is the manual's P-code
; table (ax=0,bc=1,ax'=2,bc'=3,vp=4,up=5,de=6,hl=7); "rp1" is the
; separate Q-code table (ax=0,vp=1,bc=2,up=3,ax'=4,de=5,bc'=6,hl=7) --
; same table shared/RegisterNames.h calls REGNAMES_RP1 and the manual
; itself uses for both the plain "rp1,#word" rows and the "rp,rp1"
; reg-to-reg rows below.
;
; "MOVW rp1,!addr16" and "MOVW !addr16,rp1", p.10-44 (the OT_RPL operand
; type) are cases 232-233 at the end of this file.
;
; ---- 94. MOVW rp1,#word -- p.10-44 ----
; B1 = 0110_0_Q2Q1Q0 (rp1="de", Q-code 5=101) = 0110_0101 = 0x65;
; B2/B3 = low/high word.
	movw	de, #0x1234
	.byte	0x65, 0x34, 0x12

; ---- 95. MOVW saddrp,#word -- p.10-44 ----
; B1 = 0000_1100 = 0x0c; B2 = saddr-offset; B3/B4 = low/high word.
	movw	0xfe30, #0x5678
	.byte	0x0c, 0x30, 0x78, 0x56

; ---- 96. MOVW sfrp,#word -- p.10-44 ----
; B1 = 0000_1011 = 0x0b; B2 = sfr-offset; B3/B4 = low/high word.
	movw	sfr[0x20], #0x0102
	.byte	0x0b, 0x20, 0x02, 0x01

; ---- 97. MOVW rp,rp1 -- p.10-44 ----
; B1 = 0010_0100 = 0x24; B2 = P2P1P0(rp="hl",P-code 7=111) 0 1
; Q2Q1Q0(rp1="bc",Q-code 2=010) -> 111_0_1_010 = 0xea.
	movw	hl, bc
	.byte	0x24, 0xea

; ---- 98. MOVW AX,saddrp -- p.10-44 ----
; B1 = 0001_1100 = 0x1c; B2 = saddr-offset.
	movw	ax, 0xfe40
	.byte	0x1c, 0x40

; ---- 99. MOVW saddrp,AX -- p.10-44 ----
; B1 = 0001_1010 = 0x1a; B2 = saddr-offset.
	movw	0xfe42, ax
	.byte	0x1a, 0x42

; ---- 100. MOVW saddrp,saddrp -- p.10-44 ----
; B1 = 0011_1100 = 0x3c; B2 = SOURCE offset (0xfe46 -> 0x46);
; B3 = DESTINATION offset (0xfe44 -> 0x44). Same layout as case 4.
	movw	0xfe44, 0xfe46
	.byte	0x3c, 0x46, 0x44

; ---- 101. MOVW AX,sfrp -- p.10-44 ----
; B1 = 0001_0001 = 0x11; B2 = sfr-offset.
	movw	ax, sfr[0x24]
	.byte	0x11, 0x24

; ---- 102. MOVW sfrp,AX -- p.10-44 ----
; B1 = 0001_0011 = 0x13; B2 = sfr-offset.
	movw	sfr[0x26], ax
	.byte	0x13, 0x26

; ---- 103. XCHW AX,saddrp -- p.10-44 ----
; B1 = 0001_1011 = 0x1b; B2 = saddr-offset.
	xchw	ax, 0xfe48
	.byte	0x1b, 0x48

; ---- 104. XCHW AX,sfrp -- p.10-44 ----
; B1 = 0000_0001 = 0x01 (prefix, same as 8-bit XCH A,sfr -- case 92);
; B2 = 0001_1011 = 0x1b; B3 = sfr-offset.
	xchw	ax, sfr[0x28]
	.byte	0x01, 0x1b, 0x28

; ---- 105. XCHW saddrp,saddrp -- p.10-44 ----
; B1 = 0010_1010 = 0x2a; B2 = second operand's offset (0x4c);
; B3 = first operand's offset (0x4a). Same layout as case 4.
	xchw	0xfe4a, 0xfe4c
	.byte	0x2a, 0x4c, 0x4a

; ---- 106. XCHW rp,rp1 -- p.10-44 ----
; B1 = 0010_0101 = 0x25; B2 = P2P1P0(rp="de",P-code 6=110) 0 1
; Q2Q1Q0(rp1="up",Q-code 3=011) -> 110_0_1_011 = 0xcb.
	xchw	de, up
	.byte	0x25, 0xcb

; =====================================================================
; GROUP: 8-bit Operation Instructions (ADD, ADDC, SUB, SUBC, AND, OR,
; XOR, CMP) -- cases 107-122.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-45 to 10-49 (PDF pages 292-296 of the bitsavers
; scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; SAMPLING RATIONALE: this is the first group where the manual's table
; is genuinely repetitive rather than merely long -- all 8 mnemonics
; share the exact same 9 operand forms (#byte, saddr/#byte, sfr/#byte,
; r/r1, A/saddr, A/sfr, saddr/saddr, A/mem, mem/A) with byte-for-byte
; identical field layouts, confirmed directly from the manual images
; (not assumed): the only thing that ever changes between mnemonics is
; one fixed 4-bit "op-select" nibble (ADD=1000, ADDC=1001, SUB=1010,
; SUBC=1011, AND=1100, OR=1110, XOR=1101, CMP=1111), and that nibble
; sits in the exact same bit position in every one of the 9 forms for a
; given mnemonic. So: ADD gets full 9-row coverage below (cases
; 107-115), exercising every operand-form's field mechanics once
; (including the saddr,saddr source/destination order and Base Mode mem
; addressing) -- and the other 7 mnemonics each get one case (their
; simplest form, A,#byte -- cases 116-122) to confirm that mnemonic's
; own op-select nibble lands correctly, since the addressing-mode
; mechanics it would share with ADD's other 8 rows are already proven
; and don't vary by mnemonic. This is a deliberate sampling choice, not
; the "every row gets a case" treatment the BT/BF/BTCLR/BFSET family
; got (cases 37-62) -- documented here, not left to be guessed at.
;
; ---- 107. ADD A,#byte -- p.10-45 ----
; B1 = 1010_1000 = 0xa8; B2 = Data.
	add	a, #0x11
	.byte	0xa8, 0x11

; ---- 108. ADD saddr,#byte -- p.10-45 ----
; B1 = 0110_1000 = 0x68; B2 = saddr-offset; B3 = Data.
	add	0xfe50, #0x22
	.byte	0x68, 0x50, 0x22

; ---- 109. ADD sfr,#byte -- p.10-45 ----
; B1 = 0000_0001 = 0x01 (prefix); B2 = 0110_1000 = 0x68; B3 = sfr-offset;
; B4 = Data.
	add	sfr[0x2a], #0x33
	.byte	0x01, 0x68, 0x2a, 0x33

; ---- 110. ADD r,r1 -- p.10-45 ----
; B1 = 1000_1000 = 0x88; B2 = R3R2R1R0(dest r="h",4-bit code 15=1111) 0
; R2R1R0(src r1="c",3-bit code 2=010) -> 1111_0_010 = 0xf2.
	add	h, c
	.byte	0x88, 0xf2

; ---- 111. ADD A,saddr -- p.10-45 ----
; B1 = 1001_1000 = 0x98; B2 = saddr-offset.
	add	a, 0xfe52
	.byte	0x98, 0x52

; ---- 112. ADD A,sfr -- p.10-45 ----
; B1 = 0000_0001 = 0x01; B2 = 1001_1000 = 0x98; B3 = sfr-offset.
	add	a, sfr[0x2c]
	.byte	0x01, 0x98, 0x2c

; ---- 113. ADD saddr,saddr -- p.10-45 ----
; B1 = 0111_1000 = 0x78; B2 = SOURCE saddr offset (0xfe56 -> 0x56);
; B3 = DESTINATION saddr offset (0xfe54 -> 0x54). Same layout as case 4.
	add	0xfe54, 0xfe56
	.byte	0x78, 0x56, 0x54

; ---- 114. ADD A,mem (Base Mode, [DE+byte]) -- p.10-45 ----
; B1 = 000_00110 (mod=Base Mode) = 0x06; B2 = dir(0, A is dest)
; mem(000=[DE+byte]) 1000(ADD's nibble) = 0_000_1000 = 0x08; B3 =
; offset. Base Mode is the only mem sub-mode this vasm lineage can
; assemble (Register Indirect/Base Index/Index modes are a documented
; GAP, see this file's header) -- same constraint already noted for the
; 8-bit Data Transfer group's mem cases above, exercised here with
; [DE+byte] and [UP+byte] instead of the [HL+byte]/[VP+byte]/[SP+byte]
; forms already covered by cases 86-87 and 90.
	add	a, [de+0x40]
	.byte	0x06, 0x08, 0x40

; ---- 115. ADD mem,A (Base Mode, [UP+byte]) -- p.10-45 ----
; B1 = 0x06; B2 = dir(1, mem is dest) mem(011=[UP+byte]) 1000 =
; 1_011_1000 = 0xb8; B3 = offset.
	add	[up+0x41], a
	.byte	0x06, 0xb8, 0x41

; ---- 116. ADDC A,#byte -- p.10-45 ----
; B1 = 1010_1001 = 0xa9 (ADDC's own op-select nibble, 1001); B2 = Data.
; Addressing-mode mechanics for every other ADDC operand form are
; identical to ADD's (cases 107-115) -- see this group's header note.
	addc	a, #0x12
	.byte	0xa9, 0x12

; ---- 117. SUB A,#byte -- p.10-46 ----
; B1 = 1010_1010 = 0xaa (SUB's nibble, 1010); B2 = Data.
	sub	a, #0x13
	.byte	0xaa, 0x13

; ---- 118. SUBC A,#byte -- p.10-46 ----
; B1 = 1010_1011 = 0xab (SUBC's nibble, 1011); B2 = Data.
	subc	a, #0x14
	.byte	0xab, 0x14

; ---- 119. AND A,#byte -- p.10-47 ----
; B1 = 1010_1100 = 0xac (AND's nibble, 1100); B2 = Data.
	and	a, #0x15
	.byte	0xac, 0x15

; ---- 120. OR A,#byte -- p.10-48 ----
; B1 = 1010_1110 = 0xae (OR's nibble, 1110); B2 = Data.
	or	a, #0x16
	.byte	0xae, 0x16

; ---- 121. XOR A,#byte -- p.10-48 ----
; B1 = 1010_1101 = 0xad (XOR's nibble, 1101); B2 = Data.
	xor	a, #0x17
	.byte	0xad, 0x17

; ---- 122. CMP A,#byte -- p.10-49 ----
; B1 = 1010_1111 = 0xaf (CMP's nibble, 1111); B2 = Data.
	cmp	a, #0x18
	.byte	0xaf, 0x18

; =====================================================================
; GROUP: 16-bit Operation Instructions (ADDW, SUBW, CMPW) -- cases
; 123-133.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-49 to 10-51 (PDF pages 296-298 of the bitsavers
; scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; SAMPLING RATIONALE: unlike the 8-bit Operation group above, this one
; does NOT reduce to "one shared op-select nibble per mnemonic" --
; checked directly against the manual rather than assumed by analogy.
; ADDW/SUBW/CMPW each have 7 operand forms (AX/#word, saddrp/#word,
; sfrp/#word, rp/rp1, AX/saddrp, AX/sfrp, saddrp/saddrp), but the
; mnemonic-select nibble for the three "#word"-style forms (ADDW=1101,
; SUBW=1110, CMPW=1111) is DIFFERENT from the nibble for the three
; "rp1"-style forms (ADDW=1000, SUBW=1010, CMPW=1111 -- note CMPW's two
; nibbles happen to coincide; ADDW's and SUBW's don't). So: ADDW gets
; full 7-row coverage (cases 123-129), and SUBW/CMPW each get TWO
; representative cases (their AX,#word and rp,rp1 forms -- cases
; 130-133) to independently confirm both nibble families, not just one.
;
; ---- 123. ADDW AX,#word -- p.10-49 ----
; B1 = 0010_1101 = 0x2d; B2/B3 = low/high word.
	addw	ax, #0x5566
	.byte	0x2d, 0x66, 0x55

; ---- 124. ADDW saddrp,#word -- p.10-49 ----
; B1 = 0000_1101 = 0x0d; B2 = saddr-offset; B3/B4 = low/high word.
	addw	0xfe5c, #0x7788
	.byte	0x0d, 0x5c, 0x88, 0x77

; ---- 125. ADDW sfrp,#word -- p.10-49 ----
; B1 = 0000_0001 = 0x01 (prefix); B2 = 0000_1101 = 0x0d; B3 = sfr-offset;
; B4/B5 = low/high word -- a 5-byte instruction, the longest form in
; this group.
	addw	sfr[0x2e], #0x99aa
	.byte	0x01, 0x0d, 0x2e, 0xaa, 0x99

; ---- 126. ADDW rp,rp1 -- p.10-50 ----
; B1 = 1000_1000 = 0x88 (note: a DIFFERENT nibble, 1000, than the
; #word-form's 1101 -- see this group's header note). B2 =
; P2P1P0(rp="up",P-code 5=101) 0 1 Q2Q1Q0(rp1="ax'",Q-code 4=100) ->
; 101_0_1_100 = 0xac.
	addw	up, ax'
	.byte	0x88, 0xac

; ---- 127. ADDW AX,saddrp -- p.10-50 ----
; B1 = 0001_1101 = 0x1d; B2 = saddr-offset.
	addw	ax, 0xfe64
	.byte	0x1d, 0x64

; ---- 128. ADDW AX,sfrp -- p.10-50 ----
; B1 = 0000_0001 = 0x01; B2 = 0001_1101 = 0x1d; B3 = sfr-offset.
	addw	ax, sfr[0x30]
	.byte	0x01, 0x1d, 0x30

; ---- 129. ADDW saddrp,saddrp -- p.10-50 ----
; B1 = 0011_1101 = 0x3d; B2 = SOURCE offset (0xfe90 -> 0x90);
; B3 = DESTINATION offset (0xfe62 -> 0x62). Same layout as case 4.
	addw	0xfe62, 0xfe90
	.byte	0x3d, 0x90, 0x62

; ---- 130. SUBW AX,#word -- p.10-50 ----
; B1 = 0010_1110 = 0x2e (SUBW's #word-form nibble, 1110); B2/B3 =
; low/high word.
	subw	ax, #0x1122
	.byte	0x2e, 0x22, 0x11

; ---- 131. SUBW rp,rp1 -- p.10-50 ----
; B1 = 1000_1010 = 0x8a (SUBW's rp,rp1-form nibble, 1010 -- different
; from its #word-form's 1110, same pattern as ADDW). B2 =
; P2P1P0(rp="bc",P-code 1=001) 0 1 Q2Q1Q0(rp1="hl",Q-code 7=111) ->
; 001_0_1_111 = 0x2f.
	subw	bc, hl
	.byte	0x8a, 0x2f

; ---- 132. CMPW AX,#word -- p.10-50 ----
; B1 = 0010_1111 = 0x2f (CMPW's #word-form nibble, 1111); B2/B3 =
; low/high word.
	cmpw	ax, #0x3344
	.byte	0x2f, 0x44, 0x33

; ---- 133. CMPW rp,rp1 -- p.10-50 ----
; B1 = 1000_1111 = 0x8f (CMPW's rp,rp1-form nibble, also 1111 -- the one
; case in this group where both nibble families coincide for the same
; mnemonic). B2 = P2P1P0(rp="vp",P-code 4=100) 0 1 Q2Q1Q0(rp1="up",
; Q-code 3=011) -> 100_0_1_011 = 0x8b.
	cmpw	vp, up
	.byte	0x8f, 0x8b

; =====================================================================
; GROUP: Multiply/Divide Instructions (MULU, DIVUW, MULUW, DIVUX) --
; cases 134-137.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-51 (PDF page 298 of the bitsavers scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; Small, complete group -- all 4 mnemonics, one operand form each, so
; full coverage rather than sampling. The table uses the manual's
; downward-arrow "same as above" convention for B1 across all 4 rows
; (re-read at 300 DPI, cropped to just this table, to trace the arrows
; with confidence rather than guess at them -- see this project's own
; prior caution about that convention): B1 = 0000_0101 = 0x05 is a
; single shared prefix byte for the whole group; B2 carries both the
; per-mnemonic op-select nibble and the register field (3-bit r1 for
; MULU/DIVUW, 3-bit rp1/Q-code for MULUW/DIVUX).
;
; ---- 134. MULU r1 -- p.10-51 ----
; B1 = 0x05; B2 = 0000_1_R2R1R0 (r1="a", code 1) = 0000_1_001 = 0x09.
	mulu	a
	.byte	0x05, 0x09

; ---- 135. DIVUW r1 -- p.10-51 ----
; B1 = 0x05 (shared); B2 = 0001_1_R2R1R0 (r1="b", code 3) = 0001_1_011
; = 0x1b.
	divuw	b
	.byte	0x05, 0x1b

; ---- 136. MULUW rp1 -- p.10-51 ----
; B1 = 0x05 (shared); B2 = 0010_1_Q2Q1Q0 (rp1="bc", Q-code 2) =
; 0010_1_010 = 0x2a.
	muluw	bc
	.byte	0x05, 0x2a

; ---- 137. DIVUX rp1 -- p.10-51 ----
; B1 = 0x05 (shared); B2 = 1110_1_Q2Q1Q0 (rp1="de", Q-code 5) =
; 1110_1_101 = 0xed.
	divux	de
	.byte	0x05, 0xed

; =====================================================================
; GROUP: Increment/Decrement Instructions (INC, DEC, INCW, DECW) --
; cases 138-145.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-51 (PDF page 298 of the bitsavers scan) -- same
; page as the Multiply/Divide group above, re-read at the same 300 DPI
; crop. Full coverage: 4 mnemonics, 2 operand forms each, none of it
; repetitive enough to sample.
;
; ---- 138. INC r1 -- p.10-51 ----
; B1 = 1100_0_R2R1R0 (r1="x'", code 4) = 1100_0_100 = 0xc4. 1-byte
; instruction (no B2) -- the register field is packed into B1 itself,
; unlike ADD/SUB/etc.'s r,r1 form which spends a whole second byte.
	inc	x'
	.byte	0xc4

; ---- 139. INC saddr -- p.10-51 ----
; B1 = 0010_0110 = 0x26; B2 = saddr-offset.
	inc	0xfe70
	.byte	0x26, 0x70

; ---- 140. DEC r1 -- p.10-51 ----
; B1 = 1100_1_R2R1R0 (r1="a'", code 5) = 1100_1_101 = 0xcd.
	dec	a'
	.byte	0xcd

; ---- 141. DEC saddr -- p.10-51 ----
; B1 = 0010_0111 = 0x27; B2 = saddr-offset.
	dec	0xfe72
	.byte	0x27, 0x72

; ---- 142. INCW rp2 -- p.10-51 ----
; B1 = 0100_01_S1S0 (rp2="hl", 2-bit S-code 3=11) = 0100_01_11 = 0x47.
; 1-byte instruction, same packed-register-in-B1 pattern as INC r1.
	incw	hl
	.byte	0x47

; ---- 143. INCW saddrp -- p.10-51 ----
; B1 = 0000_0111 = 0x07 (prefix); B2 = 1110_1000 = 0xe8 (fixed, no
; register field -- saddrp carries the operand); B3 = saddr-offset.
	incw	0xfe74
	.byte	0x07, 0xe8, 0x74

; ---- 144. DECW rp2 -- p.10-51 ----
; B1 = 0100_11_S1S0 (rp2="de", 2-bit S-code 2=10) = 0100_11_10 = 0x4e.
	decw	de
	.byte	0x4e

; ---- 145. DECW saddrp -- p.10-51 ----
; B1 = 0x07 (prefix); B2 = 1110_1001 = 0xe9; B3 = saddr-offset.
	decw	0xfe76
	.byte	0x07, 0xe9, 0x76

; =====================================================================
; GROUP: Shift/Rotation Instructions (ROR, ROL, RORC, ROLC, SHR, SHL,
; SHRW, SHLW, ROR4, ROL4) -- cases 146-155.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-51 to 10-52 (PDF pages 298-299 of the bitsavers
; scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; Full coverage, all 10 mnemonics -- this table's downward-arrow
; inheritance is unusually dense (re-read at 300 DPI, cropped tight,
; same discipline as the Multiply/Divide group) and genuinely
; ambiguous if read carelessly: ROR/RORC/SHR all share B1=0x30, and
; ROL/ROLC/SHL all share B1=0x31 -- B1's low bit alone only tells you
; rotate-right-family vs rotate-left-family (bit0=0 vs 1), NOT which of
; the three mnemonics in that family you've got. The real
; discriminator is B2's own top 2 bits (01=plain rotate, 00=rotate
; through carry, 10=shift), which the manual's table packs into the
; SAME byte as the n/register fields rather than giving it a dedicated
; row of its own -- easy to miss if you only skim B1.
;
; ---- 146. ROR r1,n -- p.10-51 ----
; B1 = 0011_0000 = 0x30 (rotate-right family: bit0=0; shared with RORC
; and SHR below). B2 = 01(plain-rotate) N2N1N0(n=3=011)
; R2R1R0(r1="c",code2=010) -> 01_011_010 = 0x5a.
	ror	c, 3
	.byte	0x30, 0x5a

; ---- 147. ROL r1,n -- p.10-51 ----
; B1 = 0011_0001 = 0x31 (rotate-left family: bit0=1; shared with ROLC
; and SHL below). B2 = 01(plain-rotate) N2N1N0(n=5=101)
; R2R1R0(r1="b",code3=011) -> 01_101_011 = 0x6b.
	rol	b, 5
	.byte	0x31, 0x6b

; ---- 148. RORC r1,n -- p.10-51 ----
; B1 = 0x30, same as ROR -- see this group's header note; the "through
; carry" distinction lives entirely in B2's top 2 bits. B2 =
; 00(through-carry) N2N1N0(n=2=010) R2R1R0(r1="a",code1=001) ->
; 00_010_001 = 0x11.
	rorc	a, 2
	.byte	0x30, 0x11

; ---- 149. ROLC r1,n -- p.10-51 ----
; B1 = 0x31, same as ROL. B2 = 00(through-carry) N2N1N0(n=6=110)
; R2R1R0(r1="x",code0=000) -> 00_110_000 = 0x30.
	rolc	x, 6
	.byte	0x31, 0x30

; ---- 150. SHR r1,n -- p.10-51 ----
; B1 = 0x30, same as ROR/RORC. B2 = 10(shift) N2N1N0(n=1=001)
; R2R1R0(r1="b",code3=011) -> 10_001_011 = 0x8b. (Tick-mark r1
; registers (x'/a'/c'/b') hit this group's first-operand parser gap,
; same as earlier groups -- cases 146-151 all use the non-tick r1
; codes 0-3 for that reason, not for lack of trying c'/b'/etc.)
	shr	b, 1
	.byte	0x30, 0x8b

; ---- 151. SHL r1,n -- p.10-51 ----
; B1 = 0x31, same as ROL/ROLC. B2 = 10(shift) N2N1N0(n=7=111)
; R2R1R0(r1="c",code2=010) -> 10_111_010 = 0xba.
	shl	c, 7
	.byte	0x31, 0xba

; ---- 152. SHRW rp1,n -- p.10-52 ----
; B1 = 0011_0000 = 0x30 (written out in full here, not an inherited
; arrow -- the word-shift forms start a fresh sub-table). B2 =
; 11(shift,word) N2N1N0(n=4=100) Q2Q1Q0(rp1="vp",Q-code 1=001) ->
; 11_100_001 = 0xe1.
	shrw	vp, 4
	.byte	0x30, 0xe1

; ---- 153. SHLW rp1,n -- p.10-52 ----
; B1 = 0011_0001 = 0x31. B2 = 11(shift,word) N2N1N0(n=2=010)
; Q2Q1Q0(rp1="hl",Q-code 7=111) -> 11_010_111 = 0xd7.
	shlw	hl, 2
	.byte	0x31, 0xd7

; ---- 154. ROR4 [rp1] -- p.10-52 ----
; B1 = 0000_0101 = 0x05 -- the SAME prefix byte as the Multiply/Divide
; group above (cases 134-137); ROR4/ROL4 are two more members of that
; "0x05 + B2 selects the real op" family (B2's upper nibble 1000 here,
; distinct from MULU/DIVUW/MULUW/DIVUX's 0000/0001/0010/1110). B2 =
; 1000_1_Q2Q1Q0 (rp1="bc", Q-code 2=010) -> 1000_1_010 = 0x8a. The
; manual's Operand column writes this as "[rp1]" (it rotates the 4-bit
; nibbles of the byte at [rp1], not rp1 itself), but the actual operand
; TYPE is plain OT_RP1, same as a bare "rp,rp1" register operand --
; confirmed against shared/Opcodes.h (OP_RP1_2, not the bracketed
; OT_RP1P used by PUSH/POP-style indirect forms) and against vasm,
; which rejects "ror4 [bc]" and accepts only the unbracketed "ror4 bc".
	ror4	bc
	.byte	0x05, 0x8a

; ---- 155. ROL4 [rp1] -- p.10-52 ----
; B1 = 0x05 (shared). B2 = 1001_1_Q2Q1Q0 (rp1="de", Q-code 5=101) ->
; 1001_1_101 = 0x9d. Same unbracketed-syntax note as case 154.
	rol4	de
	.byte	0x05, 0x9d

; =====================================================================
; GROUP: BCD Adjustment Instruction (ADJ4) -- case 156.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-52 (PDF page 299 of the bitsavers scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; MANUAL/TABLE DISCREPANCY, RESOLVED AGAINST REAL SILICON: Chapter
; 10.3's OP-code table (p.10-52, same page as cases 152-155 above, read
; and re-read at 300 DPI with zero ambiguity in the image itself) prints
; ADJ4 as B1 = 0000_1000 = 0x08. shared/Opcodes.h has always encoded it
; as 0x04 ({ M_ADJ4, 1, 0b00000100, ... }), and vasm's real encoder
; agrees (confirmed: assembling a bare "adj4" emits byte 0x04, not
; 0x08). Rather than either blindly trusting the table (this stress
; test's whole purpose is to NOT do that) or blindly trusting
; Opcodes.h, this was checked a third way: the disassembly of the
; Kenwood TK-x4x mask ROM contains
; five real, silicon-derived occurrences of raw byte 0x04 at
; 0x168f/0x169b/0x16c7/0x16d5/0x16e2, and at least the first two sit in
; unmistakable packed-BCD-arithmetic context -- e.g. 0x168f:
;   5d          mov   a, [hl]
;   1609        addc  a, [de+]
;   04          adj4            <- byte 0x04, right after an ADDC,
;   51          mov   [hl+], a     inside a dbnz-counted byte loop
; (load a byte, add-with-carry a second byte, decimal-adjust, store,
; loop) is the textbook idiom a decimal-adjust instruction exists for,
; and the "clr1 cy" / "clr1 pswl.3" pair immediately before the loop is
; exactly the flag setup a multi-byte BCD-add routine needs. No
; plausible alternative instruction reading makes equivalent sense at
; that position (byte 0x04 doesn't partially match any other row in
; Opcodes.h either -- it's ADJ4's alone). That's strong enough,
; independent-of-the-disputed-byte evidence to treat 0x08 as a manual
; erratum and 0x04 as correct -- a single mistyped/misprinted nibble
; (1000 for 0100) is also just a very easy typesetting error to make.
; (I also went looking for this instruction's own prose writeup in
; section 10.6.8, "BCD Adjustment Instruction," p.10-154, as a possible
; second independent check. This particular scan's page images are
; locally out of printed-page order in that region of the file (e.g.
; PDF page 405 prints footer "10-93", a lower number than PDF page
; 401's "10-97" three pages earlier), so simple offset-math navigation
; from the Chapter 10.3 anchors didn't reliably land on it -- the user
; supplied the correct location, PDF page 346. That page is indeed
; section 10.6.8's ADJ4 writeup: a Condition/Operation decision table
; (keyed on the SUB/AC/CY flags) and a Flag-operation table describing
; the decimal-adjust logic itself. It does NOT list ADJ4's opcode byte
; anywhere -- consistent with the other 10.6.x prose pages already
; read in this file (e.g. BRKCS, ADD), which describe function and
; flag behavior but leave the raw encoding to the Chapter 10.3 tables.
; So this page confirms ADJ4's existence and behavior but can't itself
; settle the 0x08-vs-0x04 question either way; the mask-ROM evidence
; above remains the deciding evidence for that.)
	adj4
	.byte	0x04

; =====================================================================
; GROUP: Bit Manipulation Instructions (MOV1, AND1, ANDI1, OR1, ORI1,
; XOR1, SET1, CLR1, NOT1) -- cases 157-180.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-52 to 10-56 (PDF pages 299, 300, 302, 301, 303 of
; the bitsavers scan, IN THAT ORDER -- see note below).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; SCAN PAGE ORDER NOTE: this is the same local-scrambling defect
; already documented for the Chapter 10.6 region (see the ADJ4 comment
; above), now also confirmed inside Chapter 10.3's own table run: PDF
; page 300 prints footer "10-53", but PDF page 301 (the very next PDF
; page) prints "10-55" -- p.10-54 is missing from that spot. It turned
; up at PDF page 302, which sits physically AFTER page 301 in the file
; but prints "10-54", i.e. BEFORE "10-55" in real reading order. Found
; by reading page 301, noticing OR1's table was missing entirely
; (XOR1's rows start cold with no OR1 section above them on that page),
; and checking the neighboring PDF pages for the missing printed page
; number. Confirmed reading order used throughout this group: PDF 299
; (10-52) -> PDF 300 (10-53) -> PDF 302 (10-54) -> PDF 301 (10-55) ->
; PDF 303 (10-56).
;
; TABLE LAYOUT NOTE: this table's "B1/B2/B3" column headers carry a
; second, smaller "B4/B5" header directly beneath them (visible at the
; top of PDF page 299). This is NOT a second set of byte positions used
; by any row in THIS group -- every row here is 1-3 bytes and only ever
; uses the B1/B2/B3 columns -- it's leftover header structure from this
; same master table template accommodating OTHER instruction groups
; elsewhere in Chapter 10.3 that need up to 5 bytes. More relevant to
; actually reading this group's rows: within the B1/B2 columns, many
; rows show only a 4-bit nibble with the other nibble left blank
; (a ditto convention meaning "same as the row above," confirmed by
; cross-checking every row below against shared/Opcodes.h's own
; byte/mask fields until the nibble-ditto reading and the table source
; agreed everywhere they overlap -- e.g. MOV1 CY,PSWH.bit's displayed
; "0 0 1 0" + "1 B2B1B0" nibble pair decodes to B1=0x02/B2=0b00001bbb,
; exactly shared/Opcodes.h line 311).
;
; MNEMONIC NOTE -- ANDI1/ORI1 are NOT "AND/OR immediate": the manual's
; own table lists them as the operand rows "AND1 CY,/saddr.bit" etc.
; (a slash before the operand, meaning "the COMPLEMENT of this bit")
; directly below AND1's/OR1's plain-bit rows, using the identical
; mnemonic-group header cell (no separate "ANDI1"/"OR1" mnemonic column
; entry printed in the manual at all for these rows). Confirmed this
; project's "ANDI1"/"ORI1" mnemonic tokens are exactly these inverted-
; bit rows, not a different operation: shared/Opcodes.h's M_ANDI1 rows
; use the identical sub-op nibble pattern (e.g. CY,saddr=0b00110) that
; the manual prints for "AND1 CY,/saddr.bit" on PDF 300, and M_ORI1
; likewise matches "OR1 CY,/saddr.bit" on PDF 302 (0b01010). XOR1 has
; no such inverted-bit row in the manual at all (its table section,
; PDF 301, lists only 6 plain-bit operand rows) -- consistent with
; shared/Opcodes.h, which has no M_XORI1/inverted XOR1 entry either,
; and architecturally necessary: XOR1's own sub-op nibble is 0b01100,
; so an inverted form would need 0b01110 -- already claimed by NOT1
; saddr.bit (shared/Opcodes.h line 369) two rows down the same table.
; vasm confirms "andi1"/"ori1" parse as real, independent mnemonics
; (vasm/cpus/78k3/mnemonics_gen.c), not operand-syntax sugar on AND1/
; OR1 -- this file writes them that way (bare mnemonic, no "/" operand
; syntax, which this assembler's parser doesn't implement for these
; rows anyway).
;
; SYNTAX NOTE -- this file uses the assembler's own bit form: for
; MOV1/AND1/ANDI1/OR1/ORI1/XOR1 the bit is attached to "cy"
; ("mov1 cy.3,0xfe30", "andi1 cy.3,0xfe30"), and SET1/CLR1/NOT1 take it
; as a separate operand ("set1 a,5"). The disassembler prints NEC's
; form, the bit after its byte ("mov1 cy, 0xfe30.3", "and1 cy,
; /0xfe30.3", "set1 a.5"), which the assembler also accepts.
;
; COVERAGE: MOV1 gets full, all-12-rows treatment (both CY<-bit and
; bit<-CY directions, all 6 addressing targets each) since it's this
; group's structurally richest mnemonic and establishes every field
; mechanic the rest of the group reuses (the CY_BIT dot-syntax, the
; saddr/sfr/a/x/pswh/pswl target forms' shared byte-position pattern).
; AND1/ANDI1/OR1/ORI1/XOR1 each get one representative CY,saddr.bit
; case (same saddr address and bit number across all five, so only the
; sub-op nibble differs between them, making the pattern easy to see).
; SET1/CLR1 get their saddr-shortcut form (a genuinely different, 2-byte
; encoding -- no 0x08 prefix -- same shape as BT/BF's saddr shortcut
; from cases 37-62) plus one longer-form case each, on top of the CY
; and A,bit forms already covered by cases 20-22. NOT1 gets the CY form
; plus both saddr and sfr forms, since it had zero prior coverage at
; all (cases 20-22 only exercised SET1/CLR1's CY and SET1's A,bit
; forms) and, like SET1/CLR1, has its own distinct short CY-form
; encoding worth exercising directly.
;
; Every byte below was hand-derived from the manual tables above, then
; independently verified by assembling this exact case list with the
; real vasm toolchain and diffing byte-for-byte -- all 24 cases matched
; on the first attempt, with no corrections needed.
;
; ---- 157. MOV1 CY,saddr.bit -- p.10-52 ----
; B1 = 0000_1000 = 0x08. B2 = 00000_B2B1B0 (bit=3=011) -> 0x03.
; B3 = saddr-offset (0xfe78-0xfe00=0x78).
	mov1	cy.3, 0xfe78
	.byte	0x08, 0x03, 0x78

; ---- 158. MOV1 CY,sfr.bit -- p.10-52 ----
; B1 = 0x08 (ditto). B2 = 00001_B2B1B0 (bit=2=010) -> 0x0a.
; B3 = sfr-offset (0x10).
	mov1	cy.2, sfr[0x10]
	.byte	0x08, 0x0a, 0x10

; ---- 159. MOV1 CY,A.bit -- p.10-52 ----
; B1 = 0000_0011 = 0x03 (A/X forms use this shorter 2-byte prefix, not
; the 0x08 3-byte one -- same shape as the saddr/sfr split seen
; throughout earlier groups). B2 = 00001_B2B1B0 (bit=5=101) -> 0x0d.
	mov1	cy.5, a
	.byte	0x03, 0x0d

; ---- 160. MOV1 CY,X.bit -- p.10-52 ----
; B1 = 0x03 (ditto). B2 = 00000_B2B1B0 (bit=1=001) -> 0x01.
	mov1	cy.1, x
	.byte	0x03, 0x01

; ---- 161. MOV1 CY,PSWH.bit -- p.10-52 ----
; B1 = 0000_0010 = 0x02 (PSWH/PSWL forms get their own 2-byte prefix).
; B2 = 00001_B2B1B0 (bit=6=110) -> 0x0e.
	mov1	cy.6, pswh
	.byte	0x02, 0x0e

; ---- 162. MOV1 CY,PSWL.bit -- p.10-52 ----
; B1 = 0x02 (ditto). B2 = 00000_B2B1B0 (bit=0=000) -> 0x00.
	mov1	cy.0, pswl
	.byte	0x02, 0x00

; ---- 163. MOV1 saddr.bit,CY -- p.10-52 ----
; Reverse direction: B1 = 0x08 (ditto). B2 = 00010_B2B1B0 (bit=4=100)
; -> 0x14 (the upper-nibble jump from 0000 to 0001 is this table's own
; "write direction" discriminator, same role AND1's plain-vs-/bit
; nibble split plays below). B3 = saddr-offset (0xfe7a-0xfe00=0x7a).
	mov1	0xfe7a, cy.4
	.byte	0x08, 0x14, 0x7a

; ---- 164. MOV1 sfr.bit,CY -- p.10-52 ----
; B1 = 0x08 (ditto). B2 = 00011_B2B1B0 (bit=7=111) -> 0x1f.
; B3 = sfr-offset (0x12).
	mov1	sfr[0x12], cy.7
	.byte	0x08, 0x1f, 0x12

; ---- 165. MOV1 A.bit,CY -- p.10-53 ----
; B1 = 0x03 (ditto). B2 = 00011_B2B1B0 (bit=2=010) -> 0x1a.
	mov1	a, cy.2
	.byte	0x03, 0x1a

; ---- 166. MOV1 X.bit,CY -- p.10-53 ----
; B1 = 0x03 (ditto). B2 = 00010_B2B1B0 (bit=3=011) -> 0x13.
	mov1	x, cy.3
	.byte	0x03, 0x13

; ---- 167. MOV1 PSWH.bit,CY -- p.10-53 ----
; B1 = 0x02 (ditto). B2 = 00011_B2B1B0 (bit=1=001) -> 0x19.
	mov1	pswh, cy.1
	.byte	0x02, 0x19

; ---- 168. MOV1 PSWL.bit,CY -- p.10-53 ----
; B1 = 0x02 (ditto). B2 = 00010_B2B1B0 (bit=5=101) -> 0x15.
	mov1	pswl, cy.5
	.byte	0x02, 0x15

; ---- 169. AND1 CY,saddr.bit -- p.10-53 ----
; B1 = 0x08. B2 = 00100_B2B1B0 (bit=3=011) -> 0x23 (same saddr address
; and bit number as ANDI1/OR1/ORI1/XOR1 below, so only this nibble
; changes across all five -- see this group's header note).
; B3 = saddr-offset (0xfe7c-0xfe00=0x7c).
	and1	cy.3, 0xfe7c
	.byte	0x08, 0x23, 0x7c

; ---- 170. ANDI1 CY,saddr.bit (manual: "AND1 CY,/saddr.bit") -- p.10-53 ----
; B1 = 0x08. B2 = 00110_B2B1B0 (bit=3=011) -> 0x33 -- the "+2" sub-op
; nibble step from AND1's 00100 is the manual's own plain-vs-complement
; encoding; see this group's header note on the ANDI1/ORI1 naming.
	andi1	cy.3, 0xfe7c
	.byte	0x08, 0x33, 0x7c

; ---- 171. OR1 CY,saddr.bit -- p.10-54 ----
; B1 = 0x08. B2 = 01000_B2B1B0 (bit=3=011) -> 0x43.
	or1	cy.3, 0xfe7c
	.byte	0x08, 0x43, 0x7c

; ---- 172. ORI1 CY,saddr.bit (manual: "OR1 CY,/saddr.bit") -- p.10-54 ----
; B1 = 0x08. B2 = 01010_B2B1B0 (bit=3=011) -> 0x53.
	ori1	cy.3, 0xfe7c
	.byte	0x08, 0x53, 0x7c

; ---- 173. XOR1 CY,saddr.bit -- p.10-55 ----
; B1 = 0x08. B2 = 01100_B2B1B0 (bit=3=011) -> 0x63. No inverted-bit
; form exists for XOR1 in the manual (or in shared/Opcodes.h) -- see
; this group's header note for why.
	xor1	cy.3, 0xfe7c
	.byte	0x08, 0x63, 0x7c

; ---- 174. SET1 saddr.bit -- p.10-55 ----
; Short 2-byte saddr form (no 0x08 prefix), same shape as BT/BF's saddr
; shortcut (cases 37-62). B1 = 10110_B2B1B0 (bit=3=011) -> 0xb3.
; B2 = saddr-offset (0xfe7e-0xfe00=0x7e).
	set1	0xfe7e, 3
	.byte	0xb3, 0x7e

; ---- 175. SET1 sfr.bit -- p.10-55 ----
; Long 3-byte form. B1 = 0x08. B2 = 10001_B2B1B0 (bit=6=110) -> 0x8e.
; B3 = sfr-offset (0x14).
	set1	sfr[0x14], 6
	.byte	0x08, 0x8e, 0x14

; ---- 176. CLR1 saddr.bit -- p.10-55 ----
; Short 2-byte saddr form, same shape as case 174. B1 = 10100_B2B1B0
; (bit=2=010) -> 0xa2. B2 = saddr-offset (0xfe80-0xfe00=0x80).
	clr1	0xfe80, 2
	.byte	0xa2, 0x80

; ---- 177. CLR1 A.bit -- p.10-55 ----
; Short 2-byte A form. B1 = 0000_0011 = 0x03. B2 = 10011_B2B1B0
; (bit=4=100) -> 0x9c.
	clr1	a, 4
	.byte	0x03, 0x9c

; ---- 178. NOT1 CY -- p.10-56 ----
; Single fixed byte, no operand, same family as cases 20-21 (SET1 CY,
; CLR1 CY) immediately above it in the manual's own table. B1 =
; 0100_0010 = 0x42. First test coverage of NOT1 at all (unlike SET1/
; CLR1, it had none before this group).
	not1	cy
	.byte	0x42

; ---- 179. NOT1 saddr.bit -- p.10-56 ----
; Long 3-byte form. B1 = 0x08. B2 = 01110_B2B1B0 (bit=5=101) -> 0x75
; (this is the exact sub-op nibble, 01110, that blocks XOR1 from having
; an inverted-bit form -- see this group's header note). B3 =
; saddr-offset (0xfe82-0xfe00=0x82).
	not1	0xfe82, 5
	.byte	0x08, 0x75, 0x82

; ---- 180. NOT1 sfr.bit -- p.10-56 ----
; B1 = 0x08. B2 = 01111_B2B1B0 (bit=1=001) -> 0x79. B3 = sfr-offset
; (0x16).
	not1	sfr[0x16], 1
	.byte	0x08, 0x79, 0x16

; =====================================================================
; GROUP: Call/Return Instructions -- cases 181-186.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-56 (PDF page 303 of the bitsavers scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; CALLT [addr5] and CALLF !addr11 are already covered (cases 15-16,
; verified independently much earlier in this file); this group adds
; the remaining Call/Return rows: CALL's !addr16/rp1/[rp1] forms, and
; the three fixed-byte control-flow instructions BRK/RET/RETI.
;
; ---- 181. CALL !addr16 -- p.10-56 ----
; B1 = 0010_1000 = 0x28. B2 = Low Addr, B3 = High Addr (0x1234 ->
; 0x34, 0x12).
	call	!0x1234
	.byte	0x28, 0x34, 0x12

; ---- 182. CALL rp1 -- p.10-56 ----
; B1 = 0000_0101 = 0x05 (prefix, same family as MULU/DIVUW/ROR4/etc.
; above). B2 = 01011_Q2Q1Q0 (rp1="bc", Q-code 2=010) -> 0x5a.
	call	bc
	.byte	0x05, 0x5a

; ---- 183. CALL [rp1] -- p.10-56 ----
; B1 = 0x05. B2 = 01111_Q2Q1Q0 (rp1="de", Q-code 5=101) -> 0x7d.
	call	[de]
	.byte	0x05, 0x7d

; ---- 184. BRK -- p.10-56 ----
; Single fixed byte. B1 = 0101_1110 = 0x5e.
	brk
	.byte	0x5e

; ---- 185. RET -- p.10-56 ----
; Single fixed byte. B1 = 0101_0110 = 0x56.
	ret
	.byte	0x56

; ---- 186. RETI -- p.10-56 ----
; Single fixed byte. B1 = 0101_0111 = 0x57.
	reti
	.byte	0x57

; =====================================================================
; GROUP: Stack Operation Instructions -- cases 187-195.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-56 to 10-57 (PDF pages 303-304 of the bitsavers
; scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; PUSH {post} and PUSHU {post} are already covered (cases 13-14, the
; original post-byte-bit-order verification); this group adds PUSH PSW,
; POP {post}/PSW, POPU {post}, the three SP<->AX/word MOVW forms, and
; the dedicated INCW SP/DECW SP rows.
;
; NOTE -- MOVW SP,#word / MOVW SP,AX / MOVW AX,SP have two table rows:
; a generic one for any SFR pointer and a dedicated SP one (SP is the
; SFR at FFFCH). Both describe the same bytes; the disassembler prints
; the SFR form, "movw sfr[SP], ..." with the register name.
;
; ---- 187. PUSH PSW -- p.10-56 ----
; Single fixed byte. B1 = 0100_1001 = 0x49.
	push	psw
	.byte	0x49

; ---- 188. POP {..} -- post-byte register list -- p.10-56 ----
; Same post-byte field as PUSH (case 13), through POP's own opcode.
; {VP,UP} -> bits 4,5 set -> 0x30. B1 = 0011_0100 = 0x34.
	pop	{vp, up}
	.byte	0x34, 0x30

; ---- 189. POP PSW -- p.10-56 ----
; Single fixed byte. B1 = 0100_1000 = 0x48.
	pop	psw
	.byte	0x48

; ---- 190. POPU {..} -- post-byte_U register list -- p.10-56 ----
; Same post-byte_U field as PUSHU (case 14). {VP,PSW} -> bits 4,5 set
; -> 0x30. B1 = 0011_0110 = 0x36.
	popu	{vp, psw}
	.byte	0x36, 0x30

; ---- 191. MOVW SP,#word -- p.10-56 ----
; B1 = 0000_1011 = 0x0b. B2 = 1111_1100 = 0xfc (fixed SP discriminator
; -- see this group's header note). B3 = Low Byte, B4 = High Byte
; (0x5678 -> 0x78, 0x56).
	movw	sp, #0x5678
	.byte	0x0b, 0xfc, 0x78, 0x56

; ---- 192. MOVW SP,AX -- p.10-56 ----
; B1 = 0001_0011 = 0x13. B2 = 0xfc (ditto).
	movw	sp, ax
	.byte	0x13, 0xfc

; ---- 193. MOVW AX,SP -- p.10-57 ----
; B1 = 0001_0001 = 0x11. B2 = 0xfc (ditto).
	movw	ax, sp
	.byte	0x11, 0xfc

; ---- 194. INCW SP -- p.10-57 ----
; B1 = 0000_0101 = 0x05 (prefix, same family as case 182 etc.). B2 =
; 1100_1000 = 0xc8 (fixed -- SP has its own dedicated INCW/DECW rows,
; distinct from the general rp2/saddrp INCW rows of cases 138-141).
	incw	sp
	.byte	0x05, 0xc8

; ---- 195. DECW SP -- p.10-57 ----
; B1 = 0x05. B2 = 1100_1001 = 0xc9.
	decw	sp
	.byte	0x05, 0xc9

; =====================================================================
; GROUP: Unconditional Branch Instructions -- cases 196-198.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-57 (PDF page 304 of the bitsavers scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; BR $addr16r (PC-relative) is already covered (case 17); this group
; adds BR's other three forms: !addr16 (absolute), rp1, and [rp1].
;
; BR is the only mnemonic with both a relative ("$addr16") and an
; absolute ("!addr16") form in the same operand slot. "br !0x9abc" must
; select the absolute form; the '!' is never valid for the relative one.
;
; ---- 196. BR !addr16 -- p.10-57 ----
; B1 = 0010_1100 = 0x2c. B2 = Low Addr, B3 = High Addr (0x9abc ->
; 0xbc, 0x9a).
	br	!0x9abc
	.byte	0x2c, 0xbc, 0x9a

; ---- 197. BR rp1 -- p.10-57 ----
; B1 = 0x05. B2 = 01001_Q2Q1Q0 (rp1="hl", Q-code 7=111) -> 0x4f.
	br	hl
	.byte	0x05, 0x4f

; ---- 198. BR [rp1] -- p.10-57 ----
; B1 = 0x05. B2 = 01101_Q2Q1Q0 (rp1="vp", Q-code 1=001) -> 0x69.
	br	[vp]
	.byte	0x05, 0x69

; =====================================================================
; GROUP: Context Switch Instructions -- cases 199-200.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," p.10-60 (PDF page 307 of the bitsavers scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; ---- 199. BRKCS RBn -- p.10-60 ----
; B1 = 0000_0101 = 0x05 (prefix). B2 = 11011_N2N1N0 (n=3=011) -> 0xdb.
	brkcs	rb3
	.byte	0x05, 0xdb

; ---- 200. RETCS !addr16 -- p.10-60 ----
; B1 = 0010_1001 = 0x29. B2 = Low Addr, B3 = High Addr (0x2468 ->
; 0x68, 0x24).
	retcs	!0x2468
	.byte	0x29, 0x68, 0x24

; =====================================================================
; GROUP: String (Block Transfer) Instructions -- cases 201-224.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-60 to 10-61 (PDF pages 307-308 of the bitsavers
; scan).
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; All twelve mnemonics in this family (MOVM/MOVBK/XCHM/XCHBK/CMPME/
; CMPBKE/CMPMNE/CMPBKNE/CMPMC/CMPBKC/CMPMNC/CMPBKNC) share the same
; shape: B1 = 0x15 (fixed prefix for the whole family), B2 = an
; operation-select byte whose bits split cleanly into "which mnemonic"
; (upper nibble-ish bits) and "auto-increment ([de+]/[hl+], OP_*_A_P)
; vs auto-decrement ([de-]/[hl-], OP_*_S_P)" (bit4: 0 for increment, 1
; for decrement). Full coverage (both forms, all twelve mnemonics) is
; given here rather than sampling, for two reasons: this is a dense,
; easy-to-mistranscribe table (see below), and it's exactly the kind of
; table this stress test exists to check row-by-row against the manual
; rather than trust by construction.
;
; In every mnemonic of this family, the [DE+] and [DE-] rows' sub-op
; nibbles differ by 0b00010000 (the auto-increment vs auto-decrement
; bit): CMPME 0b00000100/0b00010100, CMPBKE 0b00100100/0b00110100, and
; so on. Case 213 (CMPMNE [DE+],A, B2 = 0000_0101 = 0x05, distinct from
; [DE-],A's 0x15) is the one the manual (p.10-61) was re-read for.
;
; ---- 201. MOVM [DE+],A -- p.10-60 ----
; B1 = 0x15. B2 = 0000_0000 = 0x00 (explicit, establishes this
; family's base pattern).
	movm	[de+], a
	.byte	0x15, 0x00

; ---- 202. MOVM [DE-],A -- p.10-60 ----
; B1 = 0x15. B2 = 0001_0000 = 0x10 (+0x10 vs case 201, the auto-
; decrement bit).
	movm	[de-], a
	.byte	0x15, 0x10

; ---- 203. MOVBK [DE+],[HL+] -- p.10-60 ----
; B1 = 0x15. B2 = 0010_0000 = 0x20.
	movbk	[de+], [hl+]
	.byte	0x15, 0x20

; ---- 204. MOVBK [DE-],[HL-] -- p.10-60 ----
; B1 = 0x15. B2 = 0011_0000 = 0x30.
	movbk	[de-], [hl-]
	.byte	0x15, 0x30

; ---- 205. XCHM [DE+],A -- p.10-60 ----
; B1 = 0x15. B2 = 0000_0001 = 0x01.
	xchm	[de+], a
	.byte	0x15, 0x01

; ---- 206. XCHM [DE-],A -- p.10-60 ----
; B1 = 0x15. B2 = 0001_0001 = 0x11.
	xchm	[de-], a
	.byte	0x15, 0x11

; ---- 207. XCHBK [DE+],[HL+] -- p.10-60 ----
; B1 = 0x15. B2 = 0010_0001 = 0x21.
	xchbk	[de+], [hl+]
	.byte	0x15, 0x21

; ---- 208. XCHBK [DE-],[HL-] -- p.10-60 ----
; B1 = 0x15. B2 = 0011_0001 = 0x31.
	xchbk	[de-], [hl-]
	.byte	0x15, 0x31

; ---- 209. CMPME [DE+],A -- p.10-60 ----
; B1 = 0x15. B2 = 0000_0100 = 0x04.
	cmpme	[de+], a
	.byte	0x15, 0x04

; ---- 210. CMPME [DE-],A -- p.10-60 ----
; B1 = 0x15. B2 = 0001_0100 = 0x14.
	cmpme	[de-], a
	.byte	0x15, 0x14

; ---- 211. CMPBKE [DE+],[HL+] -- p.10-61 ----
; B1 = 0x15. B2 = 0010_0100 = 0x24.
	cmpbke	[de+], [hl+]
	.byte	0x15, 0x24

; ---- 212. CMPBKE [DE-],[HL-] -- p.10-61 ----
; B1 = 0x15. B2 = 0011_0100 = 0x34.
	cmpbke	[de-], [hl-]
	.byte	0x15, 0x34

; ---- 213. CMPMNE [DE+],A -- p.10-61 ----
; B1 = 0x15. B2 = 0000_0101 = 0x05 (case 214, [DE-],A, is 0x15).
	cmpmne	[de+], a
	.byte	0x15, 0x05

; ---- 214. CMPMNE [DE-],A -- p.10-61 ----
; B1 = 0x15. B2 = 0001_0101 = 0x15 (this row was already correct).
	cmpmne	[de-], a
	.byte	0x15, 0x15

; ---- 215. CMPBKNE [DE+],[HL+] -- p.10-61 ----
; B1 = 0x15. B2 = 0010_0101 = 0x25.
	cmpbkne	[de+], [hl+]
	.byte	0x15, 0x25

; ---- 216. CMPBKNE [DE-],[HL-] -- p.10-61 ----
; B1 = 0x15. B2 = 0011_0101 = 0x35.
	cmpbkne	[de-], [hl-]
	.byte	0x15, 0x35

; ---- 217. CMPMC [DE+],A -- p.10-61 ----
; B1 = 0x15. B2 = 0000_0111 = 0x07.
	cmpmc	[de+], a
	.byte	0x15, 0x07

; ---- 218. CMPMC [DE-],A -- p.10-61 ----
; B1 = 0x15. B2 = 0001_0111 = 0x17.
	cmpmc	[de-], a
	.byte	0x15, 0x17

; ---- 219. CMPBKC [DE+],[HL+] -- p.10-61 ----
; B1 = 0x15. B2 = 0010_0111 = 0x27.
	cmpbkc	[de+], [hl+]
	.byte	0x15, 0x27

; ---- 220. CMPBKC [DE-],[HL-] -- p.10-61 ----
; B1 = 0x15. B2 = 0011_0111 = 0x37.
	cmpbkc	[de-], [hl-]
	.byte	0x15, 0x37

; ---- 221. CMPMNC [DE+],A -- p.10-61 ----
; B1 = 0x15. B2 = 0000_0110 = 0x06.
	cmpmnc	[de+], a
	.byte	0x15, 0x06

; ---- 222. CMPMNC [DE-],A -- p.10-61 ----
; B1 = 0x15. B2 = 0001_0110 = 0x16.
	cmpmnc	[de-], a
	.byte	0x15, 0x16

; ---- 223. CMPBKNC [DE+],[HL+] -- p.10-61 ----
; B1 = 0x15. B2 = 0010_0110 = 0x26.
	cmpbknc	[de+], [hl+]
	.byte	0x15, 0x26

; ---- 224. CMPBKNC [DE-],[HL-] -- p.10-61 ----
; B1 = 0x15. B2 = 0011_0110 = 0x36.
	cmpbknc	[de-], [hl-]
	.byte	0x15, 0x36

; =====================================================================
; GROUP: CPU Control Instructions -- cases 225-231.
;
; Document: NEC uPD78312A User's Manual (uPD78312A/uPD78310A/uPD78P312A,
; 16/8-Bit Single-Chip Microcomputers), Chapter 10.3 "OP Code of Each
; Instruction," pp.10-61 to 10-62 (PDF pages 308-309 of the bitsavers
; scan). This is the LAST page of Chapter 10.3's OP-code table --
; cases 1-231 now cover every instruction group in it, in the manual's
; own table order throughout.
; Bitsavers copy: https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf
;
; MOV STBC,#byte / MOV WDM,#byte are 4-byte instructions, not the
; 3-byte "prefix+discriminator+data" shape every other MOV-with-
; immediate row in this table uses. The manual's own Operand/B3/B4
; columns show why: B3 is labeled "Data" with an OVERLINE (complement
; notation) and B4 is labeled "Data" plain -- i.e. the byte right after
; the opcode pair is the ONE'S COMPLEMENT of the data byte, and the
; real data follows after it. This is a hardware guard against
; corrupting these two protected control registers (watchdog mode,
; standby control) from a runaway or glitched write. The assembler
; writes the complement byte itself; every "mov wdm/stbc,#imm" in the
; TK-x4x mask ROM (82 of them) follows this pattern. Cases 225/226 below
; exercise it, with the complement byte computed by hand.
;
; ---- 225. MOV STBC,#byte -- p.10-61 ----
; B1 = 0000_1001 = 0x09. B2 = 0100_0100 = 0x44 (STBC discriminator).
; B3 = ~Data = ~0x55 = 0xaa. B4 = Data = 0x55.
	mov	stbc, #0x55
	.byte	0x09, 0x44, 0xaa, 0x55

; ---- 226. MOV WDM,#byte -- p.10-61 ----
; B1 = 0x09. B2 = 0100_0010 = 0x42 (WDM discriminator). B3 = ~Data =
; ~0xaa = 0x55. B4 = Data = 0xaa.
	mov	wdm, #0xaa
	.byte	0x09, 0x42, 0x55, 0xaa

; ---- 227. SWRS -- p.10-61 ----
; Single fixed byte. B1 = 0100_0011 = 0x43.
	swrs
	.byte	0x43

; ---- 228. SEL RBn -- p.10-62 ----
; B1 = 0x05 (prefix). B2 = 10101_N2N1N0 (n=5=101) -> 0xad. The
; manual's second SEL form, "SEL RBn,ALT" (B2 base 10111), isn't
; tested: the assembler has no syntax for it, and no ROM instance is
; known to check one against.
	sel	rb5
	.byte	0x05, 0xad

; ---- 229. NOP -- p.10-62 ----
; Single fixed byte. B1 = 0000_0000 = 0x00.
	nop
	.byte	0x00

; ---- 230. EI -- p.10-62 ----
; Single fixed byte. B1 = 0100_1011 = 0x4b.
	ei
	.byte	0x4b

; ---- 231. DI -- p.10-62 ----
; Single fixed byte. B1 = 0100_1010 = 0x4a. This is the last row of
; Chapter 10.3's OP-code table.
	di
	.byte	0x4a

; ---- 232. MOVW rp1,!addr16 -- p.10-44 ----
; The rpl field is the same 3-bit Q2Q1Q0 register field as rp1 (same
; byte position, same mask). B1 = 0000_1001
; = 0x09; B2 = 1000_0_Q2Q1Q0 (rp1="de", Q-code 5=101) = 1000_0101 =
; 0x85; B3/B4 = low/high addr16.
	movw	de, !0x1234
	.byte	0x09, 0x85, 0x34, 0x12

; ---- 233. MOVW !addr16,rp1 -- p.10-44 ----
; Same row family as #232, addr16-first direction. B1 = 0x09; B2 =
; 1001_0_Q2Q1Q0 (rp1="de", Q-code 5=101) = 1001_0101 = 0x95; B3/B4 =
; low/high addr16.
	movw	!0x5678, de
	.byte	0x09, 0x95, 0x78, 0x56
