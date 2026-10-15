#!/bin/bash
# --source round trips: the output must reassemble (companion vasm 78k3
# backend) to the identical bytes. The listing/database tests that need
# no assembler are in run_listing_tests.sh.
#
#   VASM=/path/to/vasm78k3_std bash tests/run_roundtrip_test.sh
#
# Without VASM set, vasm78k3_std is looked up in PATH; if it isn't found
# the test is skipped (exit 0), since this repo doesn't contain the
# assembler. The random-image part needs python3 and is skipped without it.
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$DIR")"
DISASM="$REPO_DIR/78k_disasm"
# VASM set but wrong is an error; not set and not in PATH skips (the
# assembler isn't distributed), unless REQUIRE_VASM=1 (make check).
if [ -n "${VASM:-}" ]; then
	if [ ! -x "$VASM" ]; then
		echo "FAIL: VASM=$VASM is not an executable assembler"
		exit 1
	fi
else
	VASM="$(command -v vasm78k3_std || true)"
	if [ -z "$VASM" ]; then
		if [ "${REQUIRE_VASM:-0}" = 1 ]; then
			echo "FAIL: vasm78k3_std not found (set VASM=/path/to/vasm78k3_std)"
			exit 1
		fi
		echo "SKIP: vasm78k3_std not found (set VASM=/path/to/vasm78k3_std)"
		exit 0
	fi
fi
if [ ! -x "$DISASM" ]; then
	make -C "$REPO_DIR" >/dev/null || exit 1
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
pass=0
fail=0

# roundtrip <name> <image> [disassembler options...]
roundtrip()
{
	local name="$1" img="$2"
	shift 2
	"$DISASM" "$@" "$img" --source "$WORK/$name.s" 2> "$WORK/$name.err"
	if "$VASM" -Fbin -quiet -I "$REPO_DIR/sfr" -o "$WORK/$name.out" "$WORK/$name.s" > "$WORK/$name.log" 2>&1 &&
		cmp -s "$WORK/$name.out" "$img"; then
		echo "PASS $name"
		pass=$((pass + 1))
	else
		echo "FAIL $name"
		grep -m3 -A1 '^error' "$WORK/$name.log" | sed 's/^/     /'
		fail=$((fail + 1))
	fi
}

# 1. The stress test binary, decoded linearly.
roundtrip stress_test "$DIR/stress_test.bin"

# 2. The synthetic mask ROM and the CALLT image: their sources must still
#    build the checked-in images (which the assembler-free tests use), and
#    the --source output, plain and with a database, must reassemble.
for t in stress_test mini_rom callt_rom; do
	if "$VASM" -Fbin -quiet -o "$WORK/$t.bin" "$DIR/$t.s" > "$WORK/$t.log" 2>&1 &&
		cmp -s "$WORK/$t.bin" "$DIR/$t.bin"; then
		echo "PASS $t.s builds tests/$t.bin"
		pass=$((pass + 1))
	else
		echo "FAIL $t.s doesn't build tests/$t.bin (rebuild it with vasm and check it in)"
		fail=$((fail + 1))
	fi
done
roundtrip mini_rom_fresh "$DIR/mini_rom.bin" -a -f
if "$DISASM" --extract "$DIR/mini_rom.lst" --db "$WORK/mini_rom.inc" 2> "$WORK/mini_extract.err"; then
	roundtrip mini_rom_db "$DIR/mini_rom.bin" -d "$WORK/mini_rom.inc"
else
	echo "FAIL mini_rom: tests/mini_rom.lst did not extract"
	fail=$((fail + 1))
fi
roundtrip callt_rom "$DIR/callt_rom.bin" -b 0x8000 --callt 0x8040 -t 0x8080

# A macro (defined in an include file) in --source output reassembles.
printf '\t.macro\tKICK_WDT\n\tmov\twdm, #0x00\n\tmov\twdm, #0x86\n\t.endm\n' > "$WORK/wd_macros.inc"
printf '\x09\x42\xff\x00\x09\x42\x79\x86\x09\x42\xff\x00\x09\x42\x79\x86\x56' > "$WORK/wd.bin"
if (cd "$WORK" && "$DISASM" wd.bin -t 0 -i wd_macros.inc -m KICK_WDT --source wd.s 2> wd.err &&
	[ "$(grep -c KICK_WDT wd.s)" = 2 ] &&
	"$VASM" -Fbin -quiet -I "$REPO_DIR/sfr" -o wd.out wd.s > wd.log 2>&1 && cmp -s wd.out wd.bin); then
	echo "PASS macro in --source"
	pass=$((pass + 1))
else
	echo "FAIL macro in --source"
	fail=$((fail + 1))
fi

# A macro with a parameter in --source output reassembles.
printf '\t.macro\tLOADA value\n\tmov\ta, #\\value\n\tnop\n\t.endm\n' > "$WORK/pm_macros.inc"
printf '\xb9\x12\x00\xb9\x34\x00\x56' > "$WORK/pm.bin"
if (cd "$WORK" && "$DISASM" pm.bin -t 0 -i pm_macros.inc -m LOADA --source pm.s 2> pm.err &&
	[ "$(grep -c 'LOADA   0x' pm.s)" = 2 ] &&
	"$VASM" -Fbin -quiet -I "$REPO_DIR/sfr" -o pm.out pm.s > pm.log 2>&1 && cmp -s pm.out pm.bin); then
	echo "PASS macro with a parameter in --source"
	pass=$((pass + 1))
else
	echo "FAIL macro with a parameter in --source"
	fail=$((fail + 1))
fi

# Bit syntax the assembler must refuse rather than assemble as something
# else: the inverted bit ("/flags.3") exists only for AND1/OR1 CY,...
for line in "mov1 cy, /flags.3" "xor1 cy, /flags.3" "mov1 /flags.3, cy" "and1 /flags.3, cy" "set1 /flags.3"; do
	printf '\t.equ\tflags, 0xFE28\n\t.org\t0x100\n\t%s\n' "$line" > "$WORK/bad.s"
	if "$VASM" -Fbin -quiet -o "$WORK/bad.bin" "$WORK/bad.s" > "$WORK/bad.log" 2>&1 < /dev/null; then
		echo "FAIL assembler accepted: $line"
		fail=$((fail + 1))
	else
		echo "PASS assembler refuses: $line"
		pass=$((pass + 1))
	fi
done

# 3. Random bytes: every decodable form, odd operand values, branches into
#    the middle of instructions. Deterministic seeds, varied bases.
if command -v python3 >/dev/null; then
	for seed in 1 2 3 4 5 6 7 8; do
		read -r size base < <(python3 -c "
import random; r=random.Random($seed)
size=r.choice([4096, 16384]); print(size, r.randrange(0, 0xFE00 - size, 2))")
		python3 -c "
import random, sys; r=random.Random($seed)
b=bytearray(r.getrandbits(8) for _ in range($size))
for _ in range(3):
    a=r.randrange(0, $size - 200); n=r.randrange(32, 200); b[a:a+n]=b'\xff'*n
sys.stdout.buffer.write(b)" > "$WORK/random_$seed.bin"
		opts=(-b "$(printf '0x%x' "$base")")
		if [ $((seed % 2)) -eq 0 ]; then
			opts+=(-f -t "$(printf '0x%x' "$base")")
		fi
		roundtrip "random_$seed" "$WORK/random_$seed.bin" "${opts[@]}"
	done
else
	echo "SKIP random images: python3 not found"
fi

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
