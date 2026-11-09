// ReSys10: MAME's TMP95C061 (src/tlcs900) with the part of MAME's device lifecycle the System 10 boards need: built
// once, reset by the board, run for a number of cycles. The board supplies its memory through m_space.
//
// run() executes cached blocks of pre-decoded instructions (s10_tmp95.cpp): an instruction is decoded once (MAME's own
// operand decoder, run in a sandbox), and executing it is a call to its MAME handler with the operands already in
// place. The on-chip timers, A/D converter and micro-DMA advance once per block instead of once per instruction.
#ifndef S10_TMP95_H
#define S10_TMP95_H

#include <memory>
#include <unordered_map>
#include <vector>

#include "emu.h" // (src/tlcs900: what MAME's headers expect first)
#include "tmp95c061.h"

struct Tmp95Cpu : tmp95c061_device
{
	explicit Tmp95Cpu(uint32_t clock);
	~Tmp95Cpu();

	void start();
	// ponytail: the serial channels (JVS / debug ports, unused by MAME's System 10 too) keep their registers across
	// CPU resets and are left out of save states
	void reset();
	void run(int cycles);
	void set_input(int line, int state) { execute_set_input(line, state); }
	// the board wrote memory the CPU may run code from (blocks decoded from it are dropped)
	void code_written(const uint8_t *host);
	// the CPU's memory was replaced as a whole (a save state): every block is dropped
	void drop_code() { flush(); }

	// tests (libretro/tests/tlcs_jit_test.cpp): the CPU's architectural state, one instruction by MAME's interpreter
	// or by its host code
	struct State {
		uint32_t xwa[4], xbc[4], xde[4], xhl[4], xix, xiy, xiz, xssp, xnsp, pc, sr, f2;
		int regbank, cycles;
		bool operator==(const State &o) const { return !memcmp(this, &o, sizeof(*this)); }
	};
	State test_state();
	void test_set_state(const State &s);
	void test_step(bool jit); // (cycles of the step in State::cycles)

	using tmp95c061_device::m_save;
	using tmp95c061_device::m_space;
	using tmp95c061_device::m_icount;
	using tmp95c061_device::m_pc;
	using tmp95c061_device::m_halted;

	// RS10_TLCS_INTERP: the MAME interpreter alone; RS10_TLCS_NOJIT: the blocks without host code (run_block);
	// RS10_TLCS_VERIFY: the interpreter, each instruction checked against the block decoder (mismatches on stderr)
	enum Mode { MODE_JIT, MODE_BLOCKS, MODE_INTERP, MODE_VERIFY } mode = MODE_JIT;

	struct Desc;
	struct Block;

private:
	void step_interp(); // one instruction through MAME's execute_run
	Block *get_block(uint32_t pc);
	Block *compile(uint32_t pc);
	bool decode(uint32_t pc, Desc &d);
	void prepare_ea(const Desc &d);
	int run_block(const Block &b);
	void verify_step();
	void flush();
	uint8_t peek(uint32_t a);
	uint8_t *host_ptr(uint32_t a);
	void mark_code(uint32_t a, uint32_t len);
	static bool ends_block(ophandler f);
	bool jit_compile(Block &b);
	static void jit_flush_code();
	static int jit_exec(Tmp95Cpu *c, const Desc *d);
	static uint32_t jit_read8(Tmp95Cpu *c, uint32_t a);
	static uint32_t jit_write8(Tmp95Cpu *c, uint32_t a, uint32_t v);
	static uint32_t jit_read16(Tmp95Cpu *c, uint32_t a);
	static uint32_t jit_read32(Tmp95Cpu *c, uint32_t a);
	static uint32_t jit_write16(Tmp95Cpu *c, uint32_t a, uint32_t v);
	static uint32_t jit_write32(Tmp95Cpu *c, uint32_t a, uint32_t v);

	std::unordered_map<uint64_t, std::unique_ptr<Block>> m_blocks;
	// code flags, one per 256 bytes of the RAM the CPU maps (m_space.page), so writes there drop the blocks
	std::unordered_map<const uint8_t *, std::vector<uint8_t>> m_code_flags; // (by 4KB host page)
	bool m_has_code = false;
	struct FastEntry { uint32_t key; Block *b; };
	FastEntry m_fast[8192] = {}; // (direct-mapped in front of m_blocks: key = pc | bank << 24 | 1 << 31)
	bool m_flush_pending = false; // code was rewritten: the cache is dropped between two blocks
	int m_jit_bank = 0;           // the register bank of the running block
};

#endif
