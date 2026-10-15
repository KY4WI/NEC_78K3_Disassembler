#!/bin/bash
# Disassemble the checked-in stress_test.bin and check that every
# consecutive pair of lines (the "real instruction" / "hand-derived raw
# bytes" pairs stress_test.s was built from -- see its own header
# comment) decodes to identical mnemonic+operand text.
#
# No assembler needed: stress_test.bin is pre-built and checked in
# alongside stress_test.s, since this repo has no assembler of its own.
# See stress_test.s's header for how to regenerate it if you change the
# source, and for what this check does and doesn't verify.
set -e
DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_DIR="$(dirname "$DIR")"
DISASM="$REPO_DIR/78k_disasm"

if [ ! -x "$DISASM" ]; then
	echo "building 78k_disasm..."
	make -C "$REPO_DIR"
fi

OUT="$("$DISASM" -N "$DIR/stress_test.bin")"

# Keep only lines with an address, and only their source part (the
# address and byte columns are the first 22 characters). Drop the
# comment a .byte stand-in line carries, then collapse whitespace so two
# instructions of different byte-length still compare equal.
mapfile -t LINES < <(echo "$OUT" | grep -E '^[0-9A-F]{4}  ' | cut -c23- | sed -E 's/[[:space:]]+/ /g; s/^ //; s/[[:space:]]+$//')

n="${#LINES[@]}"
if [ "$n" -eq 0 ] || [ $((n % 2)) -ne 0 ]; then
	echo "FAIL: disassembler produced $n lines (expected a positive even number -- every test case is a real+raw PAIR)"
	exit 1
fi

pass=0
fail=0
i=0
pairnum=0
while [ "$i" -lt "$n" ]; do
	pairnum=$((pairnum+1))
	a="${LINES[$i]}"
	b="${LINES[$((i+1))]}"
	if [ "$a" = "$b" ]; then
		echo "PASS pair $pairnum: $a"
		pass=$((pass+1))
	else
		echo "FAIL pair $pairnum: real [$a] != raw [$b]"
		fail=$((fail+1))
	fi
	i=$((i+2))
done

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
