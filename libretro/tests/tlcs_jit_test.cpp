// The TLCS-900/H host code computes what MAME's interpreter computes.
//
// Every instruction a System 10 sub CPU program actually ran (a RAM dump and its list of executed addresses, from
// RS10_RAMDUMP and RS10_TLCS_NOJIT=1 RS10_TLCS_PROF=<file>) is run from many random register states, once by MAME's
// interpreter and once by the block host code; registers, flags, PC, cycles and the RAM must come out the same.
//   tlcs_jit_test <ram dump> <dump base address> <executed addresses file> [states per instruction] [mapped]
// "mapped" maps the dump as RAM pages (the host code's direct accesses) instead of logged callbacks.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "s10_tmp95.h"

static std::vector<uint8_t> load(const char *path)
{
	std::vector<uint8_t> v;
	if (FILE *f = fopen(path, "rb")) {
		for (int c; (c = fgetc(f)) != EOF; )
			v.push_back(uint8_t(c));
		fclose(f);
	}
	return v;
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: tlcs_jit_test dump base pcs [states]\n");
		return 2;
	}
	const std::vector<uint8_t> image = load(argv[1]);
	const uint32_t base = strtoul(argv[2], nullptr, 0);
	const int states = argc > 4 ? atoi(argv[4]) : 32;
	const bool mapped = argc > 5 && !strcmp(argv[5], "mapped");
	std::vector<uint32_t> pcs;
	if (FILE *f = fopen(argv[3], "r")) {
		unsigned pc;
		unsigned long long n;
		while (fscanf(f, "%x %llu", &pc, &n) == 2)
			pcs.push_back(pc);
		fclose(f);
	}
	if (image.empty() || pcs.empty()) {
		fprintf(stderr, "tlcs_jit_test: nothing to test\n");
		return 2;
	}

	// the dump where it was taken, every access through callbacks that log the writes (compared, then undone);
	// memory elsewhere reads 0 (the board's devices are not here)
	static std::vector<uint8_t> ram;
	static std::vector<std::pair<uint32_t, uint8_t>> log; // (address, value written)
	static std::vector<std::pair<uint32_t, uint8_t>> undo;
	static uint32_t s_base;
	ram = image;
	s_base = base;
	Tmp95Cpu cpu(50'745'600);
	cpu.m_space.ext_r = [] (offs_t a) -> u8 { return a - s_base < ram.size() ? ram[a - s_base] : 0; };
	cpu.m_space.ext_w = [] (offs_t a, u8 d) {
		log.push_back({ a, d });
		if (a - s_base < ram.size()) {
			undo.push_back({ a, ram[a - s_base] });
			ram[a - s_base] = d;
		}
	};
	auto revert = [&] {
		for (size_t i = undo.size(); i-- > 0; )
			ram[undo[i].first - s_base] = undo[i].second;
		undo.clear();
	};
	if (mapped) {
		cpu.m_space.ext_r = nullptr;
		cpu.m_space.ext_w = nullptr;
		cpu.m_space.map_ram(base, base + uint32_t(ram.size()) - 1, ram.data());
	}
	cpu.start();
	cpu.reset();
	std::vector<uint8_t> ram_ref;
	auto restore = [&] {
		if (mapped)
			memcpy(ram.data(), image.data(), ram.size());
		else
			revert();
	};

	std::mt19937 rng(1234);
	auto r32 = [&] { return uint32_t(rng()); };
	int failures = 0, tested = 0;
	for (uint32_t pc : pcs) {
		for (int k = 0; k < states; k++) {
			Tmp95Cpu::State s{};
			for (int i = 0; i < 4; i++)
				s.xwa[i] = r32(), s.xbc[i] = r32(), s.xde[i] = r32(), s.xhl[i] = r32();
			// pointer registers mostly inside the dump, so memory operands touch it
			auto ptr = [&] { return (k & 1) ? r32() : base + (r32() % uint32_t(ram.size() - 64)); };
			s.xix = ptr(), s.xiy = ptr(), s.xiz = ptr(), s.xssp = ptr(), s.xnsp = ptr();
			s.pc = pc;
			s.regbank = int(r32() & 3);
			s.sr = 0xf800u | (uint32_t(s.regbank) << 8) | (r32() & 0xff); // (system mode, interrupts masked)
			s.f2 = r32() & 0xff;
			s.cycles = 0;

			log.clear();
			cpu.test_set_state(s);
			cpu.test_step(false);
			const Tmp95Cpu::State want = cpu.test_state();
			const auto log_ref = log;
			if (mapped)
				ram_ref = ram;
			restore();

			log.clear();
			cpu.test_set_state(s);
			cpu.test_step(true);
			const Tmp95Cpu::State got = cpu.test_state();
			const bool mem_differs = mapped ? ram != ram_ref : log != log_ref;
			restore();
			tested++;
			if (!(got == want) || mem_differs) {
				if (failures++ < 30) {
					fprintf(stderr, "pc %06x (bytes", pc);
					for (int i = 0; i < 6; i++)
						fprintf(stderr, " %02x", image[pc - base + i]);
					fprintf(stderr, "): ");
					if (got.pc != want.pc) fprintf(stderr, "pc %x/%x ", got.pc, want.pc);
					if (got.sr != want.sr) fprintf(stderr, "sr %x/%x ", got.sr, want.sr);
					if (got.cycles != want.cycles) fprintf(stderr, "cycles %d/%d ", got.cycles, want.cycles);
					for (int i = 0; i < 4; i++) {
						if (got.xwa[i] != want.xwa[i]) fprintf(stderr, "xwa%d %x/%x ", i, got.xwa[i], want.xwa[i]);
						if (got.xbc[i] != want.xbc[i]) fprintf(stderr, "xbc%d %x/%x ", i, got.xbc[i], want.xbc[i]);
						if (got.xde[i] != want.xde[i]) fprintf(stderr, "xde%d %x/%x ", i, got.xde[i], want.xde[i]);
						if (got.xhl[i] != want.xhl[i]) fprintf(stderr, "xhl%d %x/%x ", i, got.xhl[i], want.xhl[i]);
					}
					if (got.xix != want.xix || got.xiy != want.xiy || got.xiz != want.xiz) fprintf(stderr, "index regs ");
					if (got.xssp != want.xssp || got.xnsp != want.xnsp) fprintf(stderr, "sp ");
					if (mem_differs) fprintf(stderr, "memory ");
					fprintf(stderr, "(host code / MAME)\n");
				}
				break; // (one report per instruction)
			}
		}
	}
	printf("tlcs_jit_test: %zu instructions, %d runs, %d failing instructions\n", pcs.size(), tested, failures);
	return failures ? 1 : 0;
}
