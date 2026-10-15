CC      = gcc
CFLAGS  = -std=c99 -Wall -Wextra -Wno-missing-field-initializers \
          -Wno-unused-function -O2

TARGET  = 78k_disasm
SRC     = nec78kiii_disassembler.c
HEADERS = Types.h Mnemonic.h Operand.h Opcode.h Opcodes.h \
          Decode.h Expr.h Util.h

.PHONY: all clean test roundtrip check

all: $(TARGET)

# Single translation unit -- the .c includes every header directly, so a
# change to any of them (a new opcode table entry, a label-loader fix,
# etc.) needs to trigger a rebuild too, not just a change to the .c itself.
$(TARGET): $(SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC)

# tests/stress_test.bin is checked in pre-built (this repo has no
# assembler of its own -- see tests/stress_test.s's header), so unlike
# a from-source test this needs nothing beyond $(TARGET) itself.
# Needs nothing but $(TARGET) and python3: the decode stress test, and the
# listing/database tests (extract, regenerate, and every refused edit) on
# the checked-in test images.
test: $(TARGET)
	bash tests/run_stress_test.sh
	bash tests/run_listing_tests.sh

# --source round trips; needs the companion assembler (skipped if it
# isn't found and VASM isn't set):
#   make roundtrip VASM=/path/to/vasm78k3_std
roundtrip: $(TARGET)
	VASM=$(VASM) bash tests/run_roundtrip_test.sh

# Everything, assembler required (release testing).
check: $(TARGET)
	bash tests/run_stress_test.sh
	bash tests/run_listing_tests.sh
	VASM=$(VASM) REQUIRE_VASM=1 bash tests/run_roundtrip_test.sh

clean:
	rm -f $(TARGET)
