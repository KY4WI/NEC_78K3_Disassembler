#!/bin/bash
# Listing and database tests that need no assembler: the checked-in test
# images (built from tests/*.s) are disassembled, extracted and
# regenerated, and every error the extractor and the output checks are
# meant to catch is provoked once. Each error case must fail, say the
# expected thing, and write no file.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$DIR")"
DISASM="$REPO_DIR/78k_disasm"
[ -x "$DISASM" ] || make -C "$REPO_DIR" >/dev/null || exit 1
command -v python3 >/dev/null || { echo "FAIL: python3 is needed for these tests"; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
cp "$REPO_DIR/sfr/uPD78312A.inc" .
pass=0
fail=0
ok() { echo "PASS $1"; pass=$((pass + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && sed 's/^/     /' "$2" | head -6; fail=$((fail + 1)); }

# edit NAME PYTHON-EXPR: writes NAME.lst, mini_rom.lst changed by the
# expression (s is the listing text)
edit()
{
	python3 - "$DIR/mini_rom.lst" "$1.lst" "$2" <<'EOF'
import sys, zlib
s = open(sys.argv[1]).read()
P = ' ' * 22
s = eval(sys.argv[3])
open(sys.argv[2], 'w').write(s)
EOF
}

# expect_fail NAME TEXT COMMAND...: the command must fail, its message must
# contain TEXT, and NAME.out must not exist afterward
expect_fail()
{
	local name="$1" text="$2"
	shift 2
	if "$@" > "$name.msg" 2>&1; then
		bad "$name (succeeded)" "$name.msg"
	elif ! grep -qF -- "$text" "$name.msg"; then
		bad "$name (message lacks: $text)" "$name.msg"
	elif [ -e "$name.out" ]; then
		bad "$name (wrote a file anyway)"
	else
		ok "$name"
	fi
}

# ---- what must work ----
expected_version="$(cat <<'EOF'
NEC 78K/III Disassembler 1.0
Copyright (C) 2026 Dave, KY4WI
Portions Copyright (C) 2021 wonst719

License: GPLv3-only
This software comes with ABSOLUTELY NO WARRANTY.
See LICENSE and LICENSE.MIT for details.
EOF
)"
if [ "$("$DISASM" --version)" = "$expected_version" ] &&
	"$DISASM" --help | grep -q "disassembler v1.0"; then
	ok "version 1.0"
else
	bad "version 1.0"
fi


# the edited mini-ROM listing extracts and regenerates exactly
if "$DISASM" --extract "$DIR/mini_rom.lst" --db mini.inc 2> e.msg &&
	"$DISASM" "$DIR/mini_rom.bin" -d mini.inc --listing mini.lst && cmp -s mini.lst "$DIR/mini_rom.lst"; then
	ok "mini_rom extract/regenerate"
else
	bad "mini_rom extract/regenerate" e.msg
fi

# a fresh first look at the CALLT image extracts and regenerates exactly
if "$DISASM" -b 0x8000 "$DIR/callt_rom.bin" --callt 0x8040 -t 0x8080 --listing callt.lst &&
	"$DISASM" --extract callt.lst --db callt.inc 2> e.msg &&
	"$DISASM" -b 0x8000 "$DIR/callt_rom.bin" -d callt.inc --listing callt2.lst && cmp -s callt.lst callt2.lst; then
	ok "callt extract/regenerate"
else
	bad "callt extract/regenerate" e.msg
fi

# a vector table given by address, and an include file's names used for
# addresses outside the image (here, a RAM byte): only the top-level
# include is printed, and the listing extracts and regenerates exactly
printf '        .include "uPD78312A.inc"\n        .equ    counter, 0xFE30\n' > ext_names.inc
if "$DISASM" "$DIR/mini_rom.bin" --vectors 0x0000 --include ext_names.inc -f --listing ext.lst &&
	grep -q "inc     counter" ext.lst && grep -q '.include "ext_names.inc"' ext.lst &&
	! grep -q '.include "uPD78312A.inc"' ext.lst && grep -q "RESET vector" ext.lst &&
	"$DISASM" --extract ext.lst --db ext.inc 2> e.msg &&
	"$DISASM" "$DIR/mini_rom.bin" -d ext.inc --listing ext2.lst && cmp -s ext.lst ext2.lst; then
	ok "vector table, include names"
else
	bad "vector table, include names" e.msg
fi

# A CALLT slot whose routine is outside the image, named by an include
# file (a firmware image calling into a mask ROM), must extract too.
printf '.equ ext_routine, 0x1F1B\n' > callt_ext_names.inc
python3 -c "
b=bytearray([0xFF]*0x100); b[0x40]=0x1B; b[0x41]=0x1F; b[0x80]=0xE0; b[0x81]=0x56
open('callt_ext.bin','wb').write(b)"
if "$DISASM" -b 0x8000 callt_ext.bin --callt 0x8040 -t 0x8080 -i callt_ext_names.inc --listing callt_ext.lst &&
	grep -q "callt   \[callt_ext_routine\]" callt_ext.lst &&
	"$DISASM" --extract callt_ext.lst --db callt_ext.inc 2> e.msg &&
	"$DISASM" -b 0x8000 callt_ext.bin -d callt_ext.inc --listing callt_ext2.lst && cmp -s callt_ext.lst callt_ext2.lst; then
	ok "callt slot named from an include file"
else
	bad "callt slot named from an include file" e.msg
fi

# A routine whose own name ends in _ and two hex digits (beep_0A, in slot
# 0CH) keeps its slot: callt_beep_0A is not the callt_NAME_0A form.
python3 -c "
b=bytearray([0xFF]*0x100); b[0x58]=0x90; b[0x59]=0x80; b[0x80]=0xEC; b[0x81]=0x56; b[0x90]=0x56
open('callt_hex.bin','wb').write(b)"
if "$DISASM" -b 0x8000 callt_hex.bin --callt 0x8040 -t 0x8080 --listing callt_hex.lst 2> e.msg &&
	sed 's/sub_8090/beep_0A/g' callt_hex.lst > callt_hexn.lst &&
	"$DISASM" --extract callt_hexn.lst --db callt_hexn.inc 2>> e.msg &&
	"$DISASM" -b 0x8000 callt_hex.bin -d callt_hexn.inc --listing callt_hexn2.lst 2>> e.msg &&
	grep -q "callt   \[callt_beep_0A\]" callt_hexn2.lst &&
	"$DISASM" --extract callt_hexn2.lst --db callt_hexn2.inc 2>> e.msg &&
	"$DISASM" -b 0x8000 callt_hex.bin -d callt_hexn2.inc --listing callt_hexn3.lst 2>> e.msg &&
	cmp -s callt_hexn2.lst callt_hexn3.lst; then
	ok "callt routine named like NAME_0A"
else
	bad "callt routine named like NAME_0A" e.msg
fi

# the cross-reference lists calls through the CALLT table under their
# routines, and every slot used, empty ones included
if "$DISASM" -b 0x8000 "$DIR/callt_rom.bin" --callt 0x8040 -t 0x8080 --xref callt.xref &&
	grep -q "^8087  sub_8087 *T 8080 T 8082$" callt.xref &&
	grep -q "^02H  callt_sub_8087_02 *-> 8087 sub_8087 *8082$" callt.xref &&
	grep -q "^05H  callt_05 *(empty) *8084$" callt.xref; then
	ok "cross-reference of CALLT"
else
	bad "cross-reference of CALLT" callt.xref
fi

# renaming only a label line is enough; old generated names are accepted
edit rename 's.replace(P + "sub_0800:\n", P + "far_sub:\n")'
if "$DISASM" --extract rename.lst --db rename.inc 2> e.msg && grep -q "older name" e.msg &&
	"$DISASM" "$DIR/mini_rom.bin" -d rename.inc --listing rename2.lst &&
	grep -q "callf   !far_sub" rename2.lst && ! grep -q sub_0800 rename2.lst; then
	ok "rename a label line"
else
	bad "rename a label line" e.msg
fi

# ---- what must be refused ----

edit duplabel 's.replace(P + "init:", P + "init2:\n" + P + "init:")'
expect_fail duplabel ".equ    init, init2" "$DISASM" --extract duplabel.lst --db duplabel.out

edit twice 's.replace(P + "        .equ    BUSY_BIT, 3", P + "        .equ    BUSY_BIT, 3\n" + P + "        .equ    BUSY_BIT, 4")'
expect_fail twice "already defined" "$DISASM" --extract twice.lst --db twice.out

edit undefined 's.replace("mov     a, #INIT_A|0x02", "mov     a, #INIT_B")'
expect_fail undefined ".equ    INIT_B, 0x12" "$DISASM" --extract undefined.lst --db undefined.out

edit wrongvalue 's.replace(".equ    INIT_A, 0x10", ".equ    INIT_A, 0x11")'
expect_fail wrongvalue "but this immediate is 0x12" "$DISASM" --extract wrongvalue.lst --db wrongvalue.out

edit number 's.replace("#0xFE00        ; stack top", "#0xFE01        ; stack top")'
expect_fail number "not 0xFE01" "$DISASM" --extract number.lst --db number.out

edit bytecol 's.replace("0040  0B FC 00 FE", "0040  0B FC 00 FD")'
expect_fail bytecol "don't match its ROM" "$DISASM" --extract bytecol.lst --db bytecol.out

edit mnemonic 's.replace("005F  56                      ret", "005F  56                      reti")'
expect_fail mnemonic "instruction was changed" "$DISASM" --extract mnemonic.lst --db mnemonic.out

edit operands 's.replace("mov     a, #INIT_A|0x02", "mov     a, #INIT_A, x")'
expect_fail operands "should have 2 operand" "$DISASM" --extract operands.lst --db operands.out

edit nobit 's.replace("bt      SHADOW.BUSY_BIT, loc_005F", "bt      SHADOW, loc_005F")'
expect_fail nobit "bit number after a dot" "$DISASM" --extract nobit.lst --db nobit.out

edit header 's.replace("; ---- End of generated header ----", "; my note\n" + P + "; ---- End of generated header ----")'
expect_fail header "generated header is rebuilt" "$DISASM" --extract header.lst --db header.out

edit quote 's.replace(".ascii  \"TEST ROM\\x00\"", ".ascii  \"TEST ROM\\x00")'
expect_fail quote "can't read this string" "$DISASM" --extract quote.lst --db quote.out

edit pastend 's.replace("1F09  FF FF FF FF ..          .space  image_end_2000-$, 0xFF", "1F09  FF FF FF FF ..          .space  image_end_2000-$, 0xFF\nFFFE  00 00 00 00             .byte   0, 0, 0, 0")'
expect_fail pastend "past FFFFH" "$DISASM" --extract pastend.lst --db pastend.out

edit romline 's.replace("; ROM 0x0000-0x1FFF, 8192 bytes", "; ROM 0xF000-0x1EFFF, 65536 bytes")'
expect_fail romline "doesn't describe a ROM" "$DISASM" --extract romline.lst --db romline.out

# a listing whose bytes and CRC line were both changed extracts, but its
# database is refused for the real ROM
python3 - "$DIR/mini_rom.lst" "$DIR/mini_rom.bin" forged.lst <<'EOF'
import sys, zlib
s = open(sys.argv[1]).read()
b = bytearray(open(sys.argv[2], 'rb').read())
b[0x43] = 0xFD
s = s.replace("0040  0B FC 00 FE             movw    sfr[SP], #0xFE00 ",
              "0040  0B FC 00 FD             movw    sfr[SP], #0xFD00 ")
s = s.replace("CRC-32 0x%08X" % (zlib.crc32(open(sys.argv[2], 'rb').read()) & 0xffffffff),
              "CRC-32 0x%08X" % (zlib.crc32(bytes(b)) & 0xffffffff))
open(sys.argv[3], 'w').write(s)
EOF
if "$DISASM" --extract forged.lst --db forged.inc 2> e.msg; then
	expect_fail forged "expects the ROM" "$DISASM" "$DIR/mini_rom.bin" -d forged.inc --listing forged.out
else
	bad "forged (extract failed)" e.msg
fi

# macros: three watchdog pairs; a branch lands inside the first, so only
# the other two are printed as the macro. Writing it out by hand keeps it
# written out (nomacro); the macro's byte lines can't be edited away.
cat > wd_macros.inc <<'EOF'
; test macro
        .macro  KICK_WDT
        mov     wdm, #0x00              ; restart the watchdog
        mov     wdm, #0x86
        .endm
EOF
python3 -c "
b=bytes([0x09,0x42,0xFF,0x00,0x09,0x42,0x79,0x86]*3+[0x14,0xEA,0x56])
open('wd.bin','wb').write(b)"
if "$DISASM" wd.bin -t 0 -f -i wd_macros.inc -m KICK_WDT --listing wd.lst &&
	[ "$(grep -c 'KICK_WDT' wd.lst)" = 2 ] && grep -q '^0008  09 42 FF 00 *KICK_WDT$' wd.lst &&
	grep -q '^000C  09 42 79 86$' wd.lst && grep -q '^0004  09 42 79 86 *mov     wdm, #0x86$' wd.lst &&
	"$DISASM" --extract wd.lst --db wd.inc --symbols wds.inc 2> e.msg && grep -q '^;@macro KICK_WDT$' wd.inc &&
	grep -q 'include "wd_macros.inc"' wd.inc && ! grep -q 'wd_macros' wds.inc &&
	"$DISASM" wd.bin -d wd.inc --listing wd2.lst && cmp -s wd.lst wd2.lst; then
	ok "macro"
else
	bad "macro" e.msg
fi
python3 - <<'EOF'
s = open('wd.lst').read()
open('wdx.lst', 'w').write(s.replace('0008  09 42 FF 00             KICK_WDT\n000C  09 42 79 86\n',
    '0008  09 42 FF 00             mov     wdm, #0x00\n000C  09 42 79 86             mov     wdm, #0x86\n'))
open('wdc.lst', 'w').write(s.replace('000C  09 42 79 86\n', '000C  09 42 79 86                                     ; here\n'))
open('wdd.lst', 'w').write(s.replace('000C  09 42 79 86\n', ''))
open('wdl.lst', 'w').write(s.replace('KICK_WDT\n', 'KICK_WDT\n' + ' ' * 22 + 'inside:\n'))
EOF
if "$DISASM" --extract wdx.lst --db wdx.inc 2> e.msg && grep -q '^;@ 0x0008 nomacro$' wdx.inc &&
	"$DISASM" wd.bin -d wdx.inc --listing wdx2.lst && cmp -s wdx.lst wdx2.lst; then
	ok "macro written out stays written out"
else
	bad "macro written out stays written out" e.msg
fi
expect_fail macrocomment "can't carry a comment" "$DISASM" --extract wdc.lst --db macrocomment.out
expect_fail macrobytes "no bytes for 0x000C" "$DISASM" --extract wdd.lst --db macrobytes.out
expect_fail macrolabel "nothing can go between" "$DISASM" --extract wdl.lst --db macrolabel.out
sed 's/#0x86/#0x87/' wd_macros.inc > wd_macros.tmp && mv wd_macros.tmp wd_macros.inc
expect_fail macrobody "aren't the macro KICK_WDT" "$DISASM" --extract wd.lst --db macrobody.out
expect_fail macroname "no macro named 'NOPE'" "$DISASM" wd.bin -t 0 -i wd_macros.inc -m NOPE --listing macroname.out

# macro parameters: \name stands for any value at its place; the values
# found become the arguments. A parameter used twice must match the same
# value both times. An argument written differently (here 0x10+2) is kept.
cat > pm_macros.inc <<'EOF'
        .macro  LOADA value
        mov     a, #\value
        nop
        .endm
        .macro  SAME v
        mov     a, #\v
        mov     x, #\v
        .endm
EOF
python3 -c "
b=bytes([0xB9,0x12,0x00, 0xB9,0x34,0x00, 0xB9,0x05,0xB8,0x05, 0xB9,0x05,0xB8,0x06, 0x56])
open('pm.bin','wb').write(b)"
if "$DISASM" pm.bin -t 0 -i pm_macros.inc -m LOADA -m SAME --listing pm.lst 2> e.msg &&
	grep -q '^0000  B9 12 *LOADA   0x12$' pm.lst && grep -q '^0002  00$' pm.lst &&
	grep -q '^0003  B9 34 *LOADA   0x34$' pm.lst && grep -q '^0006  B9 05 *SAME    0x05$' pm.lst &&
	grep -q '^000A  B9 05 *mov     a, #0x05$' pm.lst &&
	"$DISASM" --extract pm.lst --db pm.inc 2>> e.msg && "$DISASM" pm.bin -d pm.inc --listing pm2.lst && cmp -s pm.lst pm2.lst; then
	ok "macro parameters"
else
	bad "macro parameters" e.msg
fi
python3 - <<'EOF'
s = open('pm.lst').read()
open('pma.lst', 'w').write(s.replace('LOADA   0x12', 'LOADA   0x10+2'))
open('pmv.lst', 'w').write(s.replace('LOADA   0x12', 'LOADA   0x13'))
open('pmn.lst', 'w').write(s.replace('LOADA   0x12', 'LOADA   0x12, 1'))
EOF
if "$DISASM" --extract pma.lst --db pma.inc 2> e.msg && grep -q '^;@ 0x0000 arg 1 0x10+2$' pma.inc &&
	"$DISASM" pm.bin -d pma.inc --listing pma2.lst && cmp -s pma.lst pma2.lst; then
	ok "macro argument kept as written"
else
	bad "macro argument kept as written" e.msg
fi
expect_fail macroargval "must have the value 0x12" "$DISASM" --extract pmv.lst --db macroargval.out
expect_fail macroargn "takes 1 argument(s), not 2" "$DISASM" --extract pmn.lst --db macroargn.out
# a bit operand: the byte and the bit number compare separately, by value
printf '        .macro  CLR3 byte\n        clr1    \\byte\\().3\n        nop\n        .endm\n' > pm_bit.inc
python3 -c "open('pmb.bin','wb').write(bytes([0xA3,0x20,0x00,0x56]))"
if "$DISASM" pmb.bin -t 0 -i pm_bit.inc -m CLR3 --listing pmb.lst 2> e.msg &&
	grep -q '^0000  A3 20 *CLR3    ram_FE20$' pmb.lst &&
	"$DISASM" --extract pmb.lst --db pmb.inc 2>> e.msg; then
	ok "macro with a bit operand"
else
	bad "macro with a bit operand" e.msg
fi
printf '        .macro  CLR3 byte\n        clr1    \\byte.3\n        .endm\n' > pm_bit2.inc
expect_fail macrobitname "write '\\byte\\().3'" "$DISASM" pmb.bin -t 0 -i pm_bit2.inc --listing macrobitname.out
printf '        .macro  M x\n        nop\n        .endm\n' > pm_unused.inc
expect_fail macrounused "isn't used in its body" "$DISASM" pm.bin -t 0 -i pm_unused.inc --listing macrounused.out
printf '        .macro  M x\n        mov a, #\\y\n        .endm\n' > pm_unknown.inc
expect_fail macrounknown "isn't a parameter of 'M'" "$DISASM" pm.bin -t 0 -i pm_unknown.inc --listing macrounknown.out

# overlapping macros: the longest match wins, and between equally long
# ones the macro defined first
cat > ov_macros.inc <<'EOF'
        .macro  SHORT v
        mov     a, #\v
        .endm
        .macro  LONG v
        mov     a, #\v
        nop
        .endm
        .macro  LONG2 v
        mov     a, #\v
        nop
        .endm
EOF
python3 -c "open('ov.bin','wb').write(bytes([0xB9,0x12,0x00, 0xB9,0x34,0x56]))"
if "$DISASM" ov.bin -t 0 -i ov_macros.inc -m SHORT -m LONG2 -m LONG --listing ov.lst 2> e.msg &&
	grep -q '^0000  B9 12 *LONG    0x12$' ov.lst && grep -q '^0003  B9 34 *SHORT   0x34$' ov.lst &&
	"$DISASM" --extract ov.lst --db ov.inc 2>> e.msg && "$DISASM" ov.bin -d ov.inc --listing ov2.lst && cmp -s ov.lst ov2.lst; then
	ok "longest macro wins"
else
	bad "longest macro wins" e.msg
fi
python3 -c "open('ov3.bin','wb').write(bytes([0xB9,0x12,0x00, 0xB9,0x34,0x00, 0xB9,0x56,0x56]))"
"$DISASM" ov3.bin -t 0 -i ov_macros.inc -m SHORT -m LONG --listing ov3.lst 2> e.msg
python3 - <<'EOF'
s = open('ov3.lst').read()
open('ov3s.lst', 'w').write(s.replace('LONG    0x12\n0002  00\n', 'SHORT   0x12\n0002  00                      nop\n', 1))
EOF
expect_fail macroshorter "a longer macro matches here" "$DISASM" --extract ov3s.lst --db macroshorter.out
printf '        .macro\n        nop\n        .endm\n' > ov_bad1.inc
expect_fail macrononame "expected '.macro NAME'" "$DISASM" ov.bin -t 0 -i ov_bad1.inc --listing macrononame.out
printf '        .macros X\n        nop\n        .endm\n' > ov_bad2.inc
expect_fail macrodirective "expected '.macro NAME'" "$DISASM" ov.bin -t 0 -i ov_bad2.inc --listing macrodirective.out

# output files: never replaced, never named twice
echo keep > existing.inc
expect_fail exists "refusing to overwrite" "$DISASM" --extract "$DIR/mini_rom.lst" --db existing.inc
[ "$(cat existing.inc)" = keep ] || bad "exists (the file was changed)"
expect_fail overdb "refusing to overwrite" "$DISASM" "$DIR/mini_rom.bin" -d mini.inc --listing mini.inc
expect_fail twonames "named for two outputs" "$DISASM" --extract "$DIR/mini_rom.lst" --db twonames.out --symbols ./twonames.out
expect_fail tolisting "refusing to overwrite" "$DISASM" --extract "$DIR/mini_rom.lst" --db "$DIR/mini_rom.lst"
expect_fail wrongrom "isn't fully loaded" "$DISASM" "$DIR/callt_rom.bin" -d mini.inc --listing wrongrom.out

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
