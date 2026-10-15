; SPDX-License-Identifier: MIT
; Copyright (c) 2026 Dave, KY4WI
;
; This file alone is under the MIT License, so it can be included in any
; program, whatever that program's license (the rest of 78k_disasm is
; GPL-3.0-only):
;
; Permission is hereby granted, free of charge, to any person obtaining a
; copy of this software and associated documentation files (the
; "Software"), to deal in the Software without restriction, including
; without limitation the rights to use, copy, modify, merge, publish,
; distribute, sublicense, and/or sell copies of the Software, and to
; permit persons to whom the Software is furnished to do so, subject to
; the following conditions:
;
; The above copyright notice and this permission notice shall be included
; in all copies or substantial portions of the Software.
;
; THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
; OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
; MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
; IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
; CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
; TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
; SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
;
; =====================================================================
; uPD78312A special function register names (partial)
;
; 78k_disasm loads this file automatically (from the sfr directory next to
; the program) and names every FF00H-FFFFH operand it covers; a database
; or listing names it with  .include "uPD78312A.inc". It is plain vasm
; source, so your own code can .include it too.
;
; This list is deliberately incomplete. Every entry below comes from a
; source that has been checked; anything not yet confirmed is left out
; rather than guessed, and simply prints as sfr[0xNN]. Sources:
;   [DOC] the uPD78312A datasheet's register tables, as collected in
;         the TK-x4x project's hardware notes and checked against real
;         mask-ROM code
;   [TBL] fixed by this disassembler's own opcode table: the dedicated
;         MOVW SP rows encode SP as SFR address FCH
;
; Not yet included, pending the datasheet's full SFR table: port mode
; registers (PM*, PMC*), PWM, capture/compare (CR*), up/down counters
; (UDC*), A/D (ADM/ADCR), interrupt mode/priority (INTM, ISPR), and
; the macro-service (MS) registers.
;
; Word registers are named at their low (even) address; a byte access
; to the high half prints as sfr[0xNN].
; =====================================================================

; --- Ports ---
.equ P0, 0xff00        ; [DOC] port 0
.equ P1, 0xff01        ; [DOC] port 1
.equ P2, 0xff02        ; [DOC] port 2 / alternate functions
.equ P3, 0xff03        ; [DOC] port 3 / alternate functions

; --- Capture ---
.equ CPT0, 0xff10      ; [DOC] capture register 0 (word)
.equ CPT1, 0xff12      ; [DOC] capture register 1 (word)

; --- System ---
.equ WDM, 0xff42       ; [DOC] watchdog timer mode (protected register)
.equ STBC, 0xff44      ; [DOC] standby control (protected register)

; --- Serial port (hardware UART) ---
.equ SCM, 0xff50       ; [DOC] serial communication mode
.equ SCC, 0xff52       ; [DOC] serial communication control
.equ BRG, 0xff53       ; [DOC] baud rate generator
.equ RXB, 0xff56       ; [DOC] receive buffer (read only)
.equ TXB, 0xff57       ; [DOC] transmit buffer (write only)

; --- Free-running counter ---
.equ FRCC, 0xff60      ; [DOC] free-running counter control

; --- Timers ---
.equ TMC0, 0xff80      ; [DOC] timer 0 control
.equ TMC1, 0xff82      ; [DOC] timer 1 control
.equ TM0, 0xff88       ; [DOC] timer 0 counter (word)
.equ MD0, 0xff8a       ; [DOC] timer 0 modulus (word)
.equ TM1, 0xff8c       ; [DOC] timer 1 counter (word)
.equ MD1, 0xff8e       ; [DOC] timer 1 modulus (word)

; --- Interrupt control registers ---
.equ CRIC00, 0xffc0    ; [DOC] up/down counter 0 match 0
.equ CRIC01, 0xffc2    ; [DOC] up/down counter 0 match 1
.equ CRIC10, 0xffc4    ; [DOC] up/down counter 1 match 0
.equ CRIC11, 0xffc6    ; [DOC] up/down counter 1 match 1
.equ EXIC0, 0xffc8     ; [DOC] external interrupt 0
.equ EXIC1, 0xffca     ; [DOC] external interrupt 1
.equ EXIC2, 0xffcc     ; [DOC] external interrupt 2
.equ TMIC0, 0xffce     ; [DOC] timer 0
.equ TMIC1, 0xffd0     ; [DOC] timer 1; bit7 IF, bit6 MK, bits0-2 PR
.equ TMIC2, 0xffd2     ; [DOC] timer 2
.equ SEIC, 0xffda      ; [DOC] serial receive error
.equ SRIC, 0xffdc      ; [DOC] serial receive
.equ STIC, 0xffde      ; [DOC] serial transmit
.equ ADIC, 0xffe0      ; [DOC] A/D conversion done
.equ TBIC, 0xffe2      ; [DOC] time base

; --- CPU registers mapped into SFR space ---
.equ SP, 0xfffc        ; [TBL] stack pointer (word)
