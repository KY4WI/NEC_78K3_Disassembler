/*
 * SPDX-License-Identifier: GPL-3.0-only
 * NEC 78K/III Disassembler 1.0
 * Copyright (c) 2026 Dave, KY4WI
 * Project includes upstream MIT-licensed material (see LICENSE.MIT).
 */

#pragma once

#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include "Types.h"
#include "Mnemonic.h"
#include "Operand.h"

typedef struct Opcode
{
	Mnemonic mnemonic;
	byte bytes;

	byte op;
	byte opMask;
	byte subOp;
	byte subOpMask;

	Operand operand1;
	Operand operand2;

	bool specialOnebyteInstr;

	Operand operand3;
} Opcode;
