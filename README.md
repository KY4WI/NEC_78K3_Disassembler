# NEC 78K/III Disassembler

A command-line disassembler for the NEC 78K/III processor family
(µPD78312A and relatives). It loads ROM and firmware images at their CPU
addresses and prints a commented listing in the style of the "Unraveled"
books: address and bytes on the left, source on the right, a comment
column, and block comments between routines.

Two things set it apart:

- **The listing is assembler source.** Without its address and byte
  columns (or written with `--source`), it assembles back to the identical
  bytes with the companion vasm 78K/III backend.
- **Annotations live in a database, not in the ROM.** Everything you add
  while studying a ROM (names, comments, constants, which bytes are code
  and which are data) is kept in one `.inc` file. You edit the listing like
  source code, and `--extract` turns it back into the database. The
  database holds no ROM bytes, so it can be shared freely, and anyone with
  their own copy of the ROM regenerates the same commented listing from it.

## Contents

1. [Quick start](#quick-start)
2. [Building and testing](#building-and-testing)
3. [Working with a database](#working-with-a-database)
4. [Starting on a new ROM](#starting-on-a-new-rom)
5. [Editing the listing](#editing-the-listing)
6. [The listing](#the-listing)
7. [Names](#names)
8. [CALLT](#callt)
9. [Macros](#macros)
10. [Cross-reference](#cross-reference)
11. [Command reference](#command-reference)
12. [The database file](#the-database-file)
13. [Assembling the output](#assembling-the-output)
14. [Background](#background)

## Quick start

```sh
make
```

Recreate a commented listing from a database and your own ROM dump (here,
the TK-x4x mask ROM's database from Kenwood_TK-x4x_Reverse_Engineering):

```sh
./78k_disasm mask_rom.bin -d tkx4x_maskrom.inc --listing maskrom.lst
```

Take a first look at a ROM nobody has annotated yet:

```sh
./78k_disasm rom.bin -a -f --listing first.lst
```

`-a` decodes the vector table at 0000H and disassembles the handlers it
points to; `-f` also follows every call and branch. With no output option,
the listing goes to the screen.

## Building and testing

Build with `make` (GCC by default, or `make CC=clang`). The result is one
program, `78k_disasm`, which finds the register names in `sfr/` next to
itself. The source uses binary integer literals and POSIX functions, so it
isn't strictly ISO C99. It is tested on Linux with GCC and Clang; other
POSIX systems should work but haven't been tried.

```sh
make test                                   # no assembler needed
make roundtrip VASM=/path/to/vasm78k3_std   # needs the vasm 78K/III backend
make check VASM=/path/to/vasm78k3_std       # both; the assembler is required
make clean
```

`make test` runs the decode stress test and the listing tests: the
checked-in test images (`tests/mini_rom.bin`, `tests/callt_rom.bin`, built
from the `.s` files next to them) are disassembled, extracted and
regenerated, and every error the extractor and the output checks catch is
provoked once; each must fail, say the right thing, and write nothing.

`make roundtrip` checks that `--source` output reassembles to the identical
bytes (the stress test, the test images and eight random images) and that
the test sources still build the checked-in images. The assembler isn't
distributed with this project, so with no `VASM` given and none on the PATH
it skips; a `VASM` that doesn't exist fails, and `make check` fails if no
assembler is found.

## Working with a database

The cycle is: regenerate the listing, edit it, extract the database.

```sh
./78k_disasm rom.bin -d rom.inc --listing rom.lst     # 1. regenerate
$EDITOR rom.lst                                     # 2. edit
rm rom.inc                                          # 3. extract
./78k_disasm --extract rom.lst --db rom.inc
```

**Output files are never replaced.** An output that already exists, or a
name given for two outputs, is refused before any work is done:
`refusing to overwrite 'rom.inc': it already exists`. Delete the old file
first (keep your files in git and nothing is ever lost). Inputs always
exist, so an input can never be written over either.

**The ROM is checked.** The database records the ROM's size and CRC-32, and
a listing is only produced for a matching ROM (`--ignore-crc` overrides). A
ROM that doesn't start at 0000H is loaded with `-b`: `-b 0x8000 fw.bin`, or
named so its base is in the file name (`fw_0x8000.bin`).

**Extracting checks everything.** `--extract` needs no ROM: it rebuilds the
bytes from the listing's own byte columns and data lines and checks them
against the CRC in the listing's header. It then regenerates the listing
from the new database and compares, line by line. If anything wouldn't come
back the same, it stops, names the line, shows what it would have become,
and writes nothing. The listing itself is never written to. If it reports
lines that use older names (after a rename, say), regenerate the listing to
bring them up to date.

`--symbols FILE` with `--extract` also writes just the names, as `.equ`
lines your own source can `.include`. Addresses the ROM expects other code
to provide (a firmware callback, say) are written commented out, since
your code defines those itself.

## Starting on a new ROM

Without a database, the disassembler decodes code from entry points and
prints everything else as data, so every byte appears exactly once:

| Option | Meaning |
| --- | --- |
| `-a` | the vector table is at 0000H: walk the handlers it points to |
| `--vectors ADDR` | the same for a vector table elsewhere (a firmware image's own, say) |
| `-t ADDR[:NAME]` | code starts here (repeatable), optionally with a label |
| `-k START-END[:FORMAT]` | these bytes are data: `byte`, `word`, `string` or `ptr` (code addresses), with `/N` items per line, e.g. `-k 0x1E00-0x1EA4:string/8` |
| `-f` | also walk every call and branch target |
| `--callt ADDR` | the CALLT table is here (see [CALLT](#callt)) |
| `-i FILE` | use the names in an include file (see [Names](#names)) |
| `-m NAME` | print matching code as a macro from an include file (see [Macros](#macros)) |

A walk ends at RET/RETI/RETCS/BRK, at the next entry point, at data, or at
a run of 32 or more 0xFF bytes (erased ROM decodes as `CALLT` but is never
code); with `-f`, also at an unconditional `BR`. Routines get a separator
rule; vector handlers get names and their vector slots get comments. With
no `-a`, `--vectors` or `-t` at all, every byte is decoded as code from the
start (erased runs excepted), which suits a quick look at a small image.

Then extract the first database and carry on as above:

```sh
./78k_disasm rom.bin -a -f --listing first.lst
./78k_disasm --extract first.lst --db rom.inc
```

These options also work with a database (`-d`), to mark more code or data,
add a vector or CALLT table, or add an include, before extracting again.

A first look at TK-x4x firmware, with the mask ROM's names for the calls
and resident RAM it uses (`tkx4x_maskrom_symbols.inc`):

```sh
./78k_disasm -b 0x8000 firmware.bin --vectors 0x8000 --callt 0x8040 \
    -i tkx4x_maskrom_symbols.inc -t 0x8080 -f --listing firmware.lst
```

That prints `call !trampoline_gpio_init` and `mov outexp_shadow, #0x25`
instead of `call !sub_1F0F` and `mov ram_FE28, #0x25`.

## Editing the listing

The listing is ordinary vasm source with two columns in front. Everything
in the source part is yours to edit, except the instructions themselves,
which come from the ROM.

- **Comments.** A line holding only a comment is a block comment; it
  belongs to the next line with an address. A comment after code is an
  end-of-line comment; more lines of it go on the following lines, lined up
  in the comment column. Text is kept exactly as written.
- **Labels.** `name:` on its own line names the address of the next line,
  and a comment after it is kept with it. Renaming a label is enough:
  places that still use the old generated name (`sub_0144`) are accepted
  and pick up the new name when the listing is regenerated.
- **Constants.** `.equ NAME, VALUE` anywhere in the listing. Write the name
  in an operand (`mov SHADOW, a`, `bt SHADOW.BUSY_BIT, loc_005F`,
  `mov a, #INIT_A|0x02`) and it is checked against the ROM's value. A name
  used at every place its address appears becomes that address's name
  everywhere; otherwise it is kept just where you wrote it. Expressions use
  C operators and precedence.
- **Aliases.** One address has one label. For a second name, write
  `.equ other_name, label_name`.
- **Data layout.** Change `.byte` lines to `.word` or `.ascii`, split or
  join lines, or write a word table's entries as labels (which makes it a
  pointer table). The bytes must stay the same.
- **Includes.** An `.include "names.inc"` line added to the header is
  loaded and kept, like `-i`.

The tool rebuilds its own lines every time: the header (everything above
`; ---- End of generated header ----`, where only `.include` lines may be
added), `.org` lines, and generated labels such as `loc_0146:`. Text added
to the header is refused rather than silently lost.

When something can't be kept, the error says what to write instead:

```text
rom.lst:289: 'delay' and 'wait65' (line 288) both label 0x0144. To keep both
  names, make this one an alias of the other by replacing this line with:
        .equ    delay, wait65
rom.lst:181: 'OUTEXP' is not defined. This address is 0xFE28; to name it,
  add this line above:
        .equ    OUTEXP, 0xFE28
```

## The listing

```text
                      ; ------------------------------------------------------------
                      ; Reset: set up the stack, initialize, then idle.
                      reset_handler:                          ; RESET vector target
0040  0B FC 00 FE             movw    sfr[SP], #0xFE00        ; stack top
0044  28 4B 00                call    !init                   ; init hardware
004F  67 67 00                movw    hl, #dat_0067
0056  73 28 06                bt      SHADOW.BUSY_BIT, loc_005F
```

- **Columns:** address, up to five bytes (longer data shows four and `..`),
  then the source. Labels sit on their own line; code is indented 8,
  operands start at 16 and end-of-line comments at 40, counted within the
  source part, so the text lines up the same with or without the columns.
- **Operands:** hex is uppercase. Absolute 16-bit operands carry NEC's `!`
  (`call !sub_1234`, `mov a, !0x8000`); SFR operands are `sfr[NAME]`; bit
  operands put the bit after its byte, NEC's way (`set1 flags.3`,
  `bt flags.BUSY, target`, `mov1 cy, flags.3`, `and1 cy, /flags.3`);
  register lists are `{ax, bc}`.
- **Stand-ins:** an instruction the assembler can't reproduce byte for byte
  (an encoding that also has a shorter form, or one that is meaningless,
  as when data is decoded as code) is written as `.byte` with the
  instruction as its first comment line.
- **Fills:** a run of 32 or more 0xFF bytes is `.space next_label-$, 0xFF`,
  followed by `.org` at that label, so fixed-address code (an entry table,
  say) can't move. A run that reaches the end of the image fills to
  `image_end_XXXX`.
- **Header:** the ROM's CRC line, the CALLT table line if there is one, the
  `.include` lines, and an `.equ` for every name the listing uses that
  isn't defined in it (RAM, other images).

## Names

| Prefix | Meaning |
| --- | --- |
| `sub_XXXX` | target of `CALL`/`CALLF`, of a trampoline (`br !` at a labeled entry), or an entry in a pointer table |
| `loc_XXXX` | target of a branch |
| `dat_XXXX` | data in a loaded image |
| `ram_XXXX` | data in FE00H-FEFFH |
| `ext_XXXX` | data outside every loaded image |
| `callt_...` | a CALLT slot (see [CALLT](#callt)) |

Your labels and global constants always win. Then come names from include
files: a name for an address outside every loaded image (resident RAM, or
another ROM's entry points) is used wherever that address appears, and a
register name (FF00H-FFFFH) wherever that address is data. Generated names
come last. Immediate values stay numbers unless you name them.

`sfr/uPD78312A.inc` holds every µPD78312A register name in the data
sheet's SFR table (Table 4-2), with bit notes from the user's manual, and is
loaded automatically. Other include files come from `-i FILE` or an
`.include` line; they are found next to the database or listing, in the
current directory, or in `sfr/`, and the listing names them without a path.
Only the includes you named are printed; one that another include already
pulls in isn't repeated. An include file holds `.equ` lines, comments and
`.macro` blocks (see [Macros](#macros)).

## CALLT

The 32-entry CALLT table can sit in more than one place (in the mask ROM's
space at 0040H, or in a firmware image at 8040H), so each database records
where it is, if there is one. `--callt ADDR` sets it, and the listing shows
it in its header as `; CALLT table at 0x8040`, which can be edited there.

With a table set, its entries print as a pointer table, one per line, and
each `callt` operand is a name for its slot, taken from the routine in it:

```text
8066  10 82                   .word   dac_write               ; slot 13H
...
8C12  F3                      callt   [callt_dac_write]
```

The name (defined in the generated header, `.equ callt_dac_write, 0x13`)
follows the routine when it is renamed; an empty slot is `callt_13`, and a
second slot with the same routine gets its slot number added. To choose a
slot's name yourself, write `.equ DAC_SLOT, 0x13` and use it in the
`callt` lines; used at every `callt` of that slot, it becomes the slot's
name everywhere. The operand's value is always the slot number, the one
thing that doesn't depend on where the table is.

## Macros

A sequence of instructions that turns up again and again (a watchdog
restart, say) can print as one line. Define it as an ordinary vasm macro
in an include file:

```text
        .macro  KICK_WDT
        mov     wdm, #0x00
        mov     wdm, #0x86
        .endm
```

and turn it on by name, once, with `-i` for the file and `-m` for the
macro:

```sh
78k_disasm firmware_0x8000.bin -d firmware.inc -i macros.inc -m KICK_WDT --listing new.lst
```

Nothing changes until you name a macro. Once it's on, every place where the
code matches its body exactly prints as the macro: the first
instruction's bytes on the macro line, then a line with just the address
and bytes of each instruction after it, so the listing still shows every
byte:

```text
                      loc_0146:
0146  09 42 FF 00             KICK_WDT                ; kick the watchdog
014A  09 42 79 86
014E  08 AF D0 F4             bf      sfr[TMIC1].7, loc_0146
```

`--source` leaves the byte lines out, and the `.include` of the macro file
in the header makes the source assemble to the same bytes.

A macro can have named parameters, written `\name` in its body. A
parameter stands for any value at its place (an immediate, an address, a
bit number), and the values found become the macro line's arguments. A
parameter used twice must have the same value both times:

```text
        .macro  EEPROM_WRITE addr, value
        mov     eeprom_addr, #\addr
        mov     0xFE2B, #\value
        callt   [0x0E]
        .endm

BAF0  3A 29 F0                EEPROM_WRITE 0xF0, 0x89
BAF3  3A 2B 89
BAF6  EE
```

An argument prints the way the disassembler would print that operand, so
an operand written as a name gives its name as the argument. Write an
argument differently in the listing (a name or an expression with the
same value) and `--extract` keeps it as written (`;@ ADDR arg N TEXT`).
Use named parameters only: vasm doesn't fill in `\1`-style ones the way
you'd expect. vasm reads a dot as part of a name, so a parameter followed
by a bit number is written `\byte\().3`.

Matching compares values, not spelling: `mov wdm, #0x00`, `MOV WDM,#0` and
a name for the same address all match. Write a macro body with numbers or
with names defined in an include file, so the assembler can resolve them;
for a CALLT slot, write its number (`callt [0x0E]`), since the generated
slot names change when routines are renamed.

Macros can overlap. Where more than one matches at the same address, the
longest one wins; between equally long ones, the macro defined first in
the include files wins. The order of the `-m` options doesn't matter. Once
a macro is placed, matching carries on after its last instruction.

A site stays written out when making it a macro would hide something: a
label or branch target, a comment or a data break on any instruction
after the first, or an operand written as a name that isn't one of the
arguments. A comment on the first
instruction moves to the macro line. To keep one particular site written
out, write its instructions out in the listing; `--extract` records that
(`;@ ADDR nomacro`) and keeps the rest. The macros a listing uses are
recorded in the database (`;@macro KICK_WDT`), so they stay on.

`--extract` checks every macro line against the bytes at its address, as
it checks instructions, so a macro edited into the wrong shape is caught.
The byte lines can't carry comments or have anything in between them and
their macro line.

## Cross-reference

`--xref FILE` writes four lists:

- every address the code uses, with its name and each instruction that
  refers to it (C call, T call through the CALLT table, J jump or branch,
  R data access);
- every immediate value, with the instructions that use it;
- every CALLT slot the code uses, with its name, the routine in it (or
  "empty", or "no CALLT table set") and each `callt` of it;
- every bit the code touches, by byte and bit, with each instruction and
  how it uses the bit (S set1, C clr1, N not1, T tested by bt/bf/btclr/bfset,
  R read into CY, W written from CY):

```text
FED7.4    ram_FED7.4                  T 02A6 C 02AA T 0300 C 0304 T 031D C 0321
```

## Command reference

```text
Disassemble:
  78k_disasm [-b ADDR] ROM [[-b ADDR] ROM ...] [options]
    -d, --db FILE        database (.inc) with names, comments and layout
    --listing FILE       write the listing (address/byte columns + source)
    --source FILE        write the same without the columns (assembles with vasm)
    --xref FILE          write a cross-reference of addresses, constants and bits
                         (with no output option, the listing goes to the screen)
    -b, --base ADDR      load the next ROM at ADDR (default 0, or from a file
                         name ending in 0xADDR, e.g. firmware_0x8000.bin)
    --ignore-crc         use the database even if the ROM doesn't match it
  Code, data and names (a first look, or added to a database):
    -a                   decode the vector table at 0000H and follow it
    --vectors ADDR       the same for a vector table at ADDR (e.g. 0x8000 in a
                         firmware image)
    -t, --entry ADDR[:NAME]  code starts here (repeatable)
    -k, --data START-END[:FORMAT]  these bytes are data (repeatable); FORMAT is
                         byte, word, string or ptr (a table of code addresses),
                         optionally /N items per line, e.g. :string/8
    -f, --follow         also disassemble every call and branch target
    --callt ADDR         the CALLT table is at ADDR in the loaded image
    -i, --include FILE   names (.equ lines) and macros to use, e.g. another
                         ROM's entry points; recorded in the database
    -N, --no-auto-labels don't generate sub_/loc_/dat_/ram_/ext_ names
    -m, --macro NAME     print the code as macro NAME (defined in an include file)
                         wherever it matches; recorded in the database

Extract (turn an edited listing into a database):
  78k_disasm --extract LISTING [--db FILE] [--symbols FILE]
    --db FILE            write the database
    --symbols FILE       write just the names, as an .include file for your code
    (with neither, the listing is only checked)

Other options:
    -h, --help           show this help
    --version            print version, copyright and license information

Addresses are hex: 0x8000, $8000 or 8000. Output files are never replaced: an
output that already exists is refused.
```

## The database file

The database is a single `.inc` file, generated by `--extract`; edit the
listing rather than the database. Its first part is plain vasm source: the
`.include` lines and one `.equ` for every name, with its comment. Its
second part is records that start with `;@`, which the assembler sees as
comments:

```text
;@rom 0x0000 8192 0x4BCFCF73        ROM base, size and CRC-32
;@callt 0x8040                      where the CALLT table is
;@global SHADOW                     name used for its address everywhere
;@slot DAC_SLOT                     name used for its CALLT slot everywhere
;@macro KICK_WDT                    a macro that is on (see Macros)
;@ 0x0040 code                      layout ranges: code, data byte/word/
;@ 0x08D7 data string                 string/ptr, with /N items per line
;@ 0x1E38 break                     a data line starts here
;@ 0x0040 text ; Reset handler      a block comment line, as written
;@ 0x0040 label reset_handler       a label line
;@ 0x004B equ SHADOW                an .equ line
;@ 0x0044 ; init hardware           an end-of-line comment line
;@ 0x004B op 1 INIT_A|0x02          what operand 1 at 0x004B is written as
;@ 0x0146 nomacro                   instructions written out, not as a macro
;@ 0xBAF0 arg 1 EE_BASE             what macro argument 1 at 0xBAF0 is written as
```

## Assembling the output

The `--source` output, or the listing with its first 22 columns removed,
assembles with the modified 78K/III backend of
[KY4WI/vasm](https://github.com/KY4WI/vasm) (std syntax module; build it
with `make -f Makefile.78k3 CPU=78k3 SYNTAX=std`).

**VASM availability:** The 78K/III backend is currently private, is not part
of an official vasm release, and contains modifications for which permission
to redistribute has not yet been obtained. The link may therefore be
inaccessible. The disassembler, database extraction and annotation workflow
work without vasm; only reassembly and the corresponding tests require the
compatible assembler.

If you have the backend, tell it where the include files are:

```sh
./78k_disasm rom.bin -d rom.inc --source rom.s
vasm78k3_std -Fbin -I /path/to/NEC_78KIII_Disassembler/sfr -o rebuilt.bin rom.s
cmp rebuilt.bin rom.bin
```

The disassembler and the vasm backend each keep their own copy of the
opcode table (`Opcodes.h` here, `cpus/78k3/shared/Opcodes.h` there). They
are identical apart from the mnemonic prefix and comments, and a fix to
one must be carried to the other by hand. After changing vasm's table,
regenerate `cpus/78k3/mnemonics_gen.c` with `cpus/78k3/regen.sh`.

## Background

### Why this version exists

This project is based on
[wonst719/nec78k3_disassembler](https://github.com/wonst719/nec78k3_disassembler),
which provided the original opcode tables and decoder. That C++ proof of
concept had a minimal command-line interface; this version was rewritten
in C for practical reverse engineering during work on Kenwood TK-x4x
(TK-840/940/941) radios. The tool itself isn't specific to those radios.

### Decoding fixes over upstream

The opcode tables were checked against NEC's documentation and real
78K/III ROM code, and these upstream decoding problems were fixed:

- `MOV`/`XCH`/`ADD`/... `saddr,saddr` (and `saddrp,saddrp`): the operands
  were shown in the wrong order. The source is the instruction's second
  byte and the destination its third, both ordinary short-direct
  addresses, with no bit 5/4 swap on either, as real ROM code confirms
  (see `d_saddr_fe()` in `Decode.h`).
- `XCH A,sfr` was decoded one byte short, which threw the disassembly out of
  step for the rest of the routine.
- `CALL [rp1]` (register-indirect) was shown as `CALL rp1`.
- `CALLF !addr11` and `BR [rp1]` had no operand output.
- `MOV STBC,#byte`, `MOV WDM,#byte`, `BRKCS RBn` and `SEL RBn,ALT` were
  undecoded.
- `MOVW rpl,!addr16` / `MOVW !addr16,rpl` printed a raw register index
  (`rpl0`, `rpl1`, ...) instead of a register name; the field is the same
  3-bit code as `rp1` and is now decoded the same way.
- `AND1`/`OR1 CY, X.bit` and `CY, /X.bit` were decoded as 3 bytes instead
  of 2, swallowing the first byte of the next instruction (MAME's 78K/III
  decoder agrees on 2).
- `PUSH`/`POP`/`PUSHU`/`POPU` register lists were printed high-bit-first
  instead of low-bit-first, the opposite of the manual's own multi-register
  example (U12117EJ2V0UM00 p.94, Figure 5-1).

### Reference documentation

NEC reference manuals for the µPD78312A and related 78K/III processors are
available from
[Bitsavers](https://bitsavers.trailing-edge.com/components/nec/uPD783xx/):

- [µPD78312A User's Manual](https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD78312A_User_Manual.pdf): the processor, peripherals and instruction set.
- [µPD7831xA datasheet with instruction encodings](https://bitsavers.trailing-edge.com/components/nec/uPD783xx/NEC_uPD7831xA_datasheet_incl_instruction_encodings.pdf): for checking opcode patterns.

These external reference documents are not included in this repository and
are not covered by its GPLv3 license.

### Attribution

This project is directly based on
[wonst719/nec78k3_disassembler](https://github.com/wonst719/nec78k3_disassembler):

- Copyright (c) 2021 wonst719
- Original project licensed under the MIT License
- Original opcode tables, operand model, mnemonic definitions, and core
  decoder design are retained and substantially extended here

The original MIT license and copyright notice are preserved in
[LICENSE.MIT](LICENSE.MIT).

Instruction decoding was also cross-checked against NEC/Renesas 78K/III
documentation and other open-source implementations, including Neil
Johnson's [dasmxx](https://github.com/nejohnson/dasmxx) and MAME's 78K/III
disassembler. Those were reference and validation sources, not the code
base of this repository.

## Copyright and license

Version 1.0 and later of this project are distributed under the **GNU General Public
License, version 3 only (GPL-3.0-only)**. See [LICENSE](LICENSE).

Copyright (c) 2026 Dave, KY4WI.

This project incorporates code originally authored by wonst719,
Copyright (c) 2021, under the MIT License. The original MIT notice and terms
are retained in [LICENSE.MIT](LICENSE.MIT). Those upstream components retain
their original MIT permissions; this combined release is GPLv3-only. Earlier
versions published under the MIT License retain their previous terms.

`sfr/uPD78312A.inc`, the register names file, is under the MIT License
(its terms are in the file itself), so it can be included in any program,
whatever that program's license.

**Copyright and generated output:** The GPLv3 license covers this tool,
not the ROM or firmware analyzed with it, and does not automatically apply
to the files it generates.

- **Disassembled code:** If the original binary is copyrighted, its code
  remains protected when expressed as assembly language. Removing the raw
  byte columns does not remove that copyright. Sharing a disassembly of
  copyrighted firmware requires permission or an applicable legal exception.
- **User-created material:** Original comments, annotations and analysis
  belong to their creators. Users may freely share their own labels and
  definitions; short identifiers, addresses and other factual information
  generally are not independently copyrightable.
- **Annotation databases:** A database containing only user-created
  annotations, labels and definitions can generally be shared separately,
  provided it does not reproduce substantial copyrighted firmware code or
  other third-party material.

The firmware copyright holder does not thereby own a user's independent
annotations, and the user does not acquire copyright in the firmware by
disassembling it.
