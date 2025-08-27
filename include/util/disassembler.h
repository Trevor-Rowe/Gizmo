#ifndef DISASSEMBLER_H
#define DISASSEMBLER_H

char* opcode_word[256] = {
    /* 0x00 - 0x0F */
    "NOP", "LD BC, NN", "LD [BC], A", "INC BC", "INC B", "DEC B", "LD B, N", "RLCA",
    "LD [NN], SP", "ADD HL, BC", "LD A, [BC]", "DEC BC", "INC C", "DEC C", "LD C, N", "RRCA",
    /* 0x10 - 0x1F */
    "STOP", "LD DE, NN", "LD [DE], A", "INC DE", "INC D", "DEC D", "LD D, N", "RLA",
    "JR N", "ADD HL, DE", "LD A, [DE]", "DEC DE", "INC E", "DEC E", "LD E, N", "RRA",
    /* 0x20 - 0x2F */
    "JR NZ, N", "LD HL, NN", "LD [HL+], A", "INC HL", "INC H", "DEC H", "LD H, N", "DAA",
    "JR Z, N", "ADD HL, HL", "LD A, [HL+]", "DEC HL", "INC L", "DEC L", "LD L, N", "CPL",
    /* 0x30 - 0x3F */
    "JR NC, N", "LD SP, NN", "LD [HL-], A", "INC SP", "INC [HL]", "DEC [HL]", "LD [HL], N", "SCF",
    "JR C, N", "ADD HL, SP", "LD A, [HL-]", "DEC SP", "INC A", "DEC A", "LD A, N", "CCF",
    /* 0x40 - 0x4F */
    "LD B, B", "LD B, C", "LD B, D", "LD B, E", "LD B, H", "LD B, L", "LD B, [HL]", "LD B, A",
    "LD C, B", "LD C, C", "LD C, D", "LD C, E", "LD C, H", "LD C, L", "LD C, [HL]", "LD C, A",
    /* 0x50 - 0x5F */
    "LD D, B", "LD D, C", "LD D, D", "LD D, E", "LD D, H", "LD D, L", "LD D, [HL]", "LD D, A",
    "LD E, B", "LD E, C", "LD E, D", "LD E, E", "LD E, H", "LD E, L", "LD E, [HL]", "LD E, A",
    /* 0x60 - 0x6F */
    "LD H, B", "LD H, C", "LD H, D", "LD H, E", "LD H, H", "LD H, L", "LD H, [HL]", "LD H, A",
    "LD L, B", "LD L, C", "LD L, D", "LD L, E", "LD L, H", "LD L, L", "LD L, [HL]", "LD L, A",
    /* 0x70 - 0x7F */
    "LD [HL], B", "LD [HL], C", "LD [HL], D", "LD [HL], E", "LD [HL], H", "LD [HL], L", "HALT", "LD [HL], A",
    "LD A, B", "LD A, C", "LD A, D", "LD A, E", "LD A, H", "LD A, L", "LD A, [HL]", "LD A, A",
    /* 0x80 - 0x8F */
    "ADD A, B", "ADD A, C", "ADD A, D", "ADD A, E", "ADD A, H", "ADD A, L", "ADD A, [HL]", "ADD A, A",
    "ADC A, B", "ADC A, C", "ADC A, D", "ADC A, E", "ADC A, H", "ADC A, L", "ADC A, [HL]", "ADC A, A",
    /* 0x90 - 0x9F */
    "SUB B", "SUB C", "SUB D", "SUB E", "SUB H", "SUB L", "SUB [HL]", "SUB A",
    "SBC A, B", "SBC A, C", "SBC A, D", "SBC A, E", "SBC A, H", "SBC A, L", "SBC A, [HL]", "SBC A, A",
    /* 0xA0 - 0xAF */
    "AND B", "AND C", "AND D", "AND E", "AND H", "AND L", "AND [HL]", "AND A",
    "XOR B", "XOR C", "XOR D", "XOR E", "XOR H", "XOR L", "XOR [HL]", "XOR A",
    /* 0xB0 - 0xBF */
    "OR B", "OR C", "OR D", "OR E", "OR H", "OR L", "OR [HL]", "OR A",
    "CP B", "CP C", "CP D", "CP E", "CP H", "CP L", "CP [HL]", "CP A",
    /* 0xC0 - 0xCF */
    "RET NZ", "POP BC", "JP NZ, NN", "JP NN", "CALL NZ, NN", "PUSH BC", "ADD A, NN", "RST 00H",
    "RET Z", "RET", "JP Z, NN", "PREFIX CB", "CALL Z, NN", "CALL NN", "ADC A, N", "RST 08H",
    /* 0xD0 - 0xDF */
    "RET NC", "POP DE", "JP NC, NN", "REG_PRINT", "CALL NC, NN", "PUSH DE", "SUB N", "RST 10H",
    "RET C", "RETI", "JP C, NN", "INVALID", "CALL C, NN", "INVALID", "SBC A, N", "RST 18H",
    /* 0xE0 - 0xEF */
    "LDH [FF00 + N], A", "POP HL", "LDH [C], A", "INVALID", "INVALID", "PUSH HL", "AND N", "RST 20H",
    "ADD SP, N", "JP [HL]", "LD [a16], A", "INVALID", "INVALID", "INVALID", "XOR d8", "RST 28H",
    /* 0xF0 - 0xFF */
    "LDH A, [FF00 + N]", "POP AF", "LDH A, [C]", "DI", "INVALID", "PUSH AF", "OR N", "RST 30H",
    "LD HL, SP + N", "LD SP, HL", "LD A, [NN]", "EI", "INTERRUPT EXECUTION", "PC PING", "CP N", "RST 38H"
};

#include <stdio.h>

char* cb_opcode_word[256] = {
    /* 0xCB00 - 0xCB0F */
    "RLC B", "RLC C", "RLC D", "RLC E", "RLC H", "RLC L", "RLC [HL]", "RLC A",
    "RRC B", "RRC C", "RRC D", "RRC E", "RRC H", "RRC L", "RRC [HL]", "RRC A",
    /* 0xCB10 - 0xCB1F */
    "RL B", "RL C", "RL D", "RL E", "RL H", "RL L", "RL [HL]", "RL A",
    "RR B", "RR C", "RR D", "RR E", "RR H", "RR L", "RR [HL]", "RR A",
    /* 0xCB20 - 0xCB2F */
    "SLA B", "SLA C", "SLA D", "SLA E", "SLA H", "SLA L", "SLA [HL]", "SLA A",
    "SRA B", "SRA C", "SRA D", "SRA E", "SRA H", "SRA L", "SRA [HL]", "SRA A",
    /* 0xCB30 - 0xCB3F */
    "SWAP B", "SWAP C", "SWAP D", "SWAP E", "SWAP H", "SWAP L", "SWAP [HL]", "SWAP A",
    "SRL B", "SRL C", "SRL D", "SRL E", "SRL H", "SRL L", "SRL [HL]", "SRL A",
    /* 0xCB40 - 0xCB4F */
    "BIT 0, B", "BIT 0, C", "BIT 0, D", "BIT 0, E", "BIT 0, H", "BIT 0, L", "BIT 0, [HL]", "BIT 0, A",
    "BIT 1, B", "BIT 1, C", "BIT 1, D", "BIT 1, E", "BIT 1, H", "BIT 1, L", "BIT 1, [HL]", "BIT 1, A",
    /* 0xCB50 - 0xCB5F */
    "BIT 2, B", "BIT 2, C", "BIT 2, D", "BIT 2, E", "BIT 2, H", "BIT 2, L", "BIT 2, [HL]", "BIT 2, A",
    "BIT 3, B", "BIT 3, C", "BIT 3, D", "BIT 3, E", "BIT 3, H", "BIT 3, L", "BIT 3, [HL]", "BIT 3, A",
    /* 0xCB60 - 0xCB6F */
    "BIT 4, B", "BIT 4, C", "BIT 4, D", "BIT 4, E", "BIT 4, H", "BIT 4, L", "BIT 4, [HL]", "BIT 4, A",
    "BIT 5, B", "BIT 5, C", "BIT 5, D", "BIT 5, E", "BIT 5, H", "BIT 5, L", "BIT 5, [HL]", "BIT 5, A",
    /* 0xCB70 - 0xCB7F */
    "BIT 6, B", "BIT 6, C", "BIT 6, D", "BIT 6, E", "BIT 6, H", "BIT 6, L", "BIT 6, [HL]", "BIT 6, A",
    "BIT 7, B", "BIT 7, C", "BIT 7, D", "BIT 7, E", "BIT 7, H", "BIT 7, L", "BIT 7, [HL)", "BIT 7, A",
    /* 0xCB80 - 0xCB8F */
    "RES 0, B", "RES 0, C", "RES 0, D", "RES 0, E", "RES 0, H", "RES 0, L", "RES 0, [HL]", "RES 0, A",
    "RES 1, B", "RES 1, C", "RES 1, D", "RES 1, E", "RES 1, H", "RES 1, L", "RES 1, [HL]", "RES 1, A",
    /* 0xCB90 - 0xCB9F */
    "RES 2, B", "RES 2, C", "RES 2, D", "RES 2, E", "RES 2, H", "RES 2, L", "RES 2, [HL]", "RES 2, A",
    "RES 3, B", "RES 3, C", "RES 3, D", "RES 3, E", "RES 3, H", "RES 3, L", "RES 3, [HL]", "RES 3, A",
    /* 0xCBA0 - 0xCBAF */
    "RES 4, B", "RES 4, C", "RES 4, D", "RES 4, E", "RES 4, H", "RES 4, L", "RES 4, [HL]", "RES 4, A",
    "RES 5, B", "RES 5, C", "RES 5, D", "RES 5, E", "RES 5, H", "RES 5, L", "RES 5, [HL]", "RES 5, A",
    /* 0xCBB0 - 0xCBBF */
    "RES 6, B", "RES 6, C", "RES 6, D", "RES 6, E", "RES 6, H", "RES 6, L", "RES 6, [HL]", "RES 6, A",
    "RES 7, B", "RES 7, C", "RES 7, D", "RES 7, E", "RES 7, H", "RES 7, L", "RES 7, [HL]", "RES 7, A",
    /* 0xCBC0 - 0xCBCF */
    "SET 0, B", "SET 0, C", "SET 0, D", "SET 0, E", "SET 0, H", "SET 0, L", "SET 0, [HL]", "SET 0, A",
    "SET 1, B", "SET 1, C", "SET 1, D", "SET 1, E", "SET 1, H", "SET 1, L", "SET 1, [HL]", "SET 1, A",
    /* 0xCBD0 - 0xCBDF */
    "SET 2, B", "SET 2, C", "SET 2, D", "SET 2, E", "SET 2, H", "SET 2, L", "SET 2, [HL]", "SET 2, A",
    "SET 3, B", "SET 3, C", "SET 3, D", "SET 3, E", "SET 3, H", "SET 3, L", "SET 3, [HL]", "SET 3, A",
    /* 0xCBE0 - 0xCBEF */
    "SET 4, B", "SET 4, C", "SET 4, D", "SET 4, E", "SET 4, H", "SET 4, L", "SET 4, [HL]", "SET 4, A",
    "SET 5, B", "SET 5, C", "SET 5, D", "SET 5, E", "SET 5, H", "SET 5, L", "SET 5, [HL]", "SET 5, A",
    /* 0xCBF0 - 0xCBFF */
    "SET 6, B", "SET 6, C", "SET 6, D", "SET 6, E", "SET 6, H", "SET 6, L", "SET 6, [HL]", "SET 6, A",
    "SET 7, B", "SET 7, C", "SET 7, D", "SET 7, E", "SET 7, H", "SET 7, L", "SET 7, [HL]", "SET 7, A"
};

#endif