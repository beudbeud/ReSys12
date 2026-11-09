// ReSys10 tools: TLCS-900/H disassembly (MAME's dasm900) of a memory dump.
//   tlcsdis dump.bin dump_base_addr start_addr [count]   (e.g. a MEM(P3) RAM dump: base 0x800000)
#include "emu.h"
#include "dasm900.h"
#include <cstdlib>
#include <sstream>
#include <vector>
#include <iostream>
int main(int argc, char **argv)
{
	if (argc < 4) { fprintf(stderr, "usage: tlcsdis dump base start [count]\n"); return 1; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) return 1;
	std::vector<u8> d;
	for (int c; (c = fgetc(f)) != EOF; ) d.push_back(u8(c));
	const u32 base = strtoul(argv[2], 0, 0);
	u32 pc = strtoul(argv[3], 0, 0);
	const int n = argc > 4 ? atoi(argv[4]) : 32;
	tlcs900_disassembler dis;
	util::disasm_interface::data_buffer buf{ d.data(), base, u32(d.size()) };
	for (int i = 0; i < n; i++) {
		std::ostringstream s;
		const u32 len = dis.disassemble(s, pc, buf, buf) & 0xffff;
		printf("%06x: ", pc);
		for (u32 k = 0; k < 6; k++) k < len ? printf("%02x ", buf.r8(pc + k)) : printf("   ");
		printf(" %s\n", s.str().c_str());
		pc += len ? len : 1;
	}
}
