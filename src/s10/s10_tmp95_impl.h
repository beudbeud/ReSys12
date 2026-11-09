// ReSys10: the block cache of Tmp95Cpu (s10_tmp95.h), shared by its decoder (s10_tmp95.cpp) and its host code
// generator (s10_tmp95_jit.cpp). BSD-3-Clause.
#ifndef S10_TMP95_IMPL_H
#define S10_TMP95_IMPL_H

#include "s10_tmp95.h"

// MAME 900tbl.hxx e_operand values (that enum lives in tlcs900.cpp's translation unit)
static constexpr int kP_M = 18, kP_R = 21;

enum EaKind : uint8_t {
	EA_NONE,      // m_ea2 left as the decoder found it (static or untouched)
	EA_CUR32,     // get_reg32_current(sel) (+ disp)
	EA_REG32,     // get_reg32(sel) (+ disp)
	EA_REG32_R8,  // get_reg32(sel) + int8_t(get_reg8(sel2))
	EA_REG32_R16, // get_reg32(sel) + int16_t(get_reg16(sel2))
	EA_PREDEC,    // reg -= step; ea = reg
	EA_POSTINC,   // ea = reg; reg += step
};

enum : uint8_t { SET_REG1 = 1, SET_REG2 = 2, SET_IMM1 = 4, SET_IMM2 = 8, SET_EA1 = 16, SET_EA2 = 32, EA1_FROM_EA2 = 64 };

struct Tmp95Cpu::Desc
{
	tlcs900_device::ophandler fn;
	uint32_t pc_next;  // m_pc once every operand byte is fetched
	uint32_t imm1, imm2, ea1, ea2;
	uint16_t reg1, reg2;
	int16_t disp;
	uint8_t op, set, ea_kind, ea_sel, ea_sel2, ea_step;
	int cycles;        // the table entries' cycles and the addressing modes' (the handlers add their own)
	uint32_t pc;       // (the instruction's address: diagnostics)
	uint32_t len;
};

struct Tmp95Cpu::Block
{
	std::vector<Desc> d;
	int (*code)(Tmp95Cpu *) = nullptr; // the block compiled to host code (s10_tmp95_jit.cpp); null: run_block
};


#endif
