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
; uPD78312A special function register names
;
; 78k_disasm loads this file automatically (from the sfr directory next to
; the program) and names every FF00H-FFFFH operand it covers; a database
; or listing names it with  .include "uPD78312A.inc". It is plain vasm
; source, so your own code can .include it too.
;
; Sources:
;   [DOC] NEC uPD78310A(A)/78312A(A) data sheet, Table 4-2 "Special
;         Function Registers" (every register listed there is here), and
;         the uPD78312A user's manual for the bit notes
;   [TBL] fixed by this disassembler's own opcode table: the dedicated
;         MOVW SP rows encode SP as SFR address FCH
;
; Word registers are named at their low (even) address; a byte access
; to the high half prints as sfr[0xNN]. FFB0H-FFBFH is the external
; access area (external memory reached by SFR addressing) and has no
; names.
; =====================================================================

; --- Ports ---
.equ P0, 0xff00        ; [DOC] port 0
.equ P1, 0xff01        ; [DOC] port 1
.equ P2, 0xff02        ; [DOC] port 2 / alternate functions (bits 0-3 read only)
.equ P3, 0xff03        ; [DOC] port 3 / alternate functions (bits 0-3 read only)
.equ P4, 0xff04        ; [DOC] port 4 (AD0-AD7 in memory expansion mode)
.equ P5, 0xff05        ; [DOC] port 5 (A8-A15 in memory expansion mode)
.equ P0L, 0xff3a       ; [DOC] port 0 buffer register, low 4 bits
.equ P0H, 0xff3b       ; [DOC] port 0 buffer register, high 4 bits

; --- Port mode and control ---
.equ PM0, 0xff20       ; [DOC] port 0 mode (1 = input), reset FFH
.equ PM1, 0xff21       ; [DOC] port 1 mode
.equ PM2, 0xff22       ; [DOC] port 2 mode
.equ PM3, 0xff23       ; [DOC] port 3 mode
.equ PM5, 0xff25       ; [DOC] port 5 mode
.equ PMC2, 0xff32      ; [DOC] port 2 mode control (1 = control pin), reset 0FH
.equ PMC3, 0xff33      ; [DOC] port 3 mode control, reset 0FH
.equ RTPC, 0xff38      ; [DOC] real-time output port control

; --- Capture/compare, PWM and up/down counters ---
.equ CR00, 0xff08      ; [DOC] capture/compare register 00 (word)
.equ CR01, 0xff0a      ; [DOC] capture/compare register 01 (word)
.equ CR10, 0xff0c      ; [DOC] capture/compare register 10 (word)
.equ CR11, 0xff0e      ; [DOC] capture/compare register 11 (word)
.equ CPT0, 0xff10      ; [DOC] capture register 0 (word)
.equ CPT1, 0xff12      ; [DOC] capture register 1 (word)
.equ PWM0, 0xff14      ; [DOC] PWM register 0 (word)
.equ PWM1, 0xff16      ; [DOC] PWM register 1 (word)
.equ UDC0, 0xff1c      ; [DOC] presettable up/down count register 0 (word)
.equ UDC1, 0xff1e      ; [DOC] presettable up/down count register 1 (word)
.equ CPTM, 0xff64      ; [DOC] capture mode
.equ PWMM, 0xff66      ; [DOC] PWM mode
.equ CUIM, 0xff70      ; [DOC] count unit input mode
.equ UDCC0, 0xff72     ; [DOC] up/down counter control 0
.equ CRC, 0xff74       ; [DOC] capture/compare register control
.equ UDCC1, 0xff7a     ; [DOC] up/down counter control 1: bit 7 count, bit 3 clock
                       ;       (0 = fCLK/3), bit 0 modulo mode (clear on CR11 match)

; --- System ---
.equ MM, 0xff40        ; [DOC] memory expansion mode: bits 2-0 area, bits 5-4 wait
.equ RFM, 0xff41       ; [DOC] refresh mode
.equ WDM, 0xff42       ; [DOC] watchdog timer mode (protected register): bit 7
                       ;       RUN, bit 4 PRC, bits 2-0 clock (86H: 349.5 ms at 6 MHz)
.equ STBC, 0xff44      ; [DOC] standby control (protected register); CK1 = 0
                       ;       gives fCLK = fXX/2
.equ TBM, 0xff46       ; [DOC] time base mode
.equ INTM, 0xff48      ; [DOC] external interrupt mode (valid edges; reset 00H:
                       ;       falling edge on INTE0-INTE2 and NMI)
.equ ISPR, 0xff4a      ; [DOC] in-service priority (read only)
.equ CCW, 0xff4e       ; [DOC] CPU control word; bit 1 TPF moves the vector and
                       ;       CALLT tables from 0000H-007FH to 8000H-807FH

; --- Serial port (hardware UART) ---
.equ SCM, 0xff50       ; [DOC] serial communication mode
.equ SCC, 0xff52       ; [DOC] serial communication control (bits 2-0: n)
.equ BRG, 0xff53       ; [DOC] baud rate generator (G): baud = fCLK / (2^(n+2) * G)
.equ RXB, 0xff56       ; [DOC] receive buffer (read only)
.equ TXB, 0xff57       ; [DOC] transmit buffer (write only)

; --- Free-running counter, A/D ---
.equ FRCC, 0xff60      ; [DOC] free-running counter control
.equ ADM, 0xff68       ; [DOC] A/D converter mode
.equ ADCR, 0xff6a      ; [DOC] A/D conversion result (read only)

; --- Timers ---
.equ TMC0, 0xff80      ; [DOC] timer 0 control
.equ TMC1, 0xff82      ; [DOC] timer 1 control
.equ TM0, 0xff88       ; [DOC] timer 0 counter (word)
.equ MD0, 0xff8a       ; [DOC] timer 0 modulus (word)
.equ TM1, 0xff8c       ; [DOC] timer 1 counter (word)
.equ MD1, 0xff8e       ; [DOC] timer 1 modulus (word)

; --- Interrupt request control (bit 7 request flag, bit 6 mask, bit 5
; --- macro service, bit 4 context switching, bits 2-0 priority) and
; --- macro service control registers
.equ CRIC00, 0xffc0    ; [DOC] count unit 0, request 0 (CRF00)
.equ CRMS00, 0xffc1    ; [DOC] count unit 0 macro service 0
.equ CRIC01, 0xffc2    ; [DOC] count unit 0, request 1 (CRF01)
.equ CRIC10, 0xffc4    ; [DOC] count unit 1, request 0 (CRF10)
.equ CRMS10, 0xffc5    ; [DOC] count unit 1 macro service 0
.equ CRIC11, 0xffc6    ; [DOC] count unit 1, request 1 (CRF11)
.equ EXIC0, 0xffc8     ; [DOC] external interrupt 0 (INTE0)
.equ EXMS0, 0xffc9     ; [DOC] external interrupt 0 macro service
.equ EXIC1, 0xffca     ; [DOC] external interrupt 1 (INTE1)
.equ EXMS1, 0xffcb     ; [DOC] external interrupt 1 macro service
.equ EXIC2, 0xffcc     ; [DOC] external interrupt 2 (INTE2)
.equ EXMS2, 0xffcd     ; [DOC] external interrupt 2 macro service
.equ TMIC0, 0xffce     ; [DOC] timer 0 (TMF0); its bits 2-0 set the priority of
                       ;       the whole timer group (TMF0 > TMF1 > TMF2)
.equ TMMS0, 0xffcf     ; [DOC] timer 0 macro service
.equ TMIC1, 0xffd0     ; [DOC] timer 1 (TMF1); no priority bits of its own
.equ TMMS1, 0xffd1     ; [DOC] timer 1 macro service
.equ TMIC2, 0xffd2     ; [DOC] timer 2 (TMF2); no priority bits of its own
.equ TMMS2, 0xffd3     ; [DOC] timer 2 macro service
.equ SEIC, 0xffda      ; [DOC] serial receive error
.equ SRIC, 0xffdc      ; [DOC] serial receive completion
.equ SRMS, 0xffdd      ; [DOC] serial receive macro service
.equ STIC, 0xffde      ; [DOC] serial send completion
.equ STMS, 0xffdf      ; [DOC] serial send macro service
.equ ADIC, 0xffe0      ; [DOC] A/D conversion done
.equ ADMS, 0xffe1      ; [DOC] A/D macro service
.equ TBIC, 0xffe2      ; [DOC] time base counter

; --- CPU registers mapped into SFR space ---
.equ SP, 0xfffc        ; [TBL] stack pointer (word)
