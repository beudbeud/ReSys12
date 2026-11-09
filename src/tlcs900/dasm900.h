// ReSys10: no TLCS-900 disassembler in this build (MAME's dasm900 is for its debugger); its interface only.
#ifndef RESYS10_DASM900_H
#define RESYS10_DASM900_H
#include "emu.h"
class tlcs900_disassembler : public util::disasm_interface
{
public:
	struct cr_sym { u8 size; u8 encoding; char const *name; };
	tlcs900_disassembler() {}
	template <size_t N> tlcs900_disassembler(std::pair<u16, char const *> const (&)[N]) {}
	template <size_t N, size_t CRN> tlcs900_disassembler(std::pair<u16, char const *> const (&)[N], cr_sym const (&)[CRN]) {}
};
#endif
