// ReSys10 tools: what MAME's dasm900.cpp needs to build as a command line disassembler.
#pragma once
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <ostream>
#include <string>
#include <algorithm>
#include <type_traits>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef uint32_t offs_t;
inline std::string string_format(const char *fmt, ...)
{ char b[256]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap); return b; }
namespace util {
using ::string_format;
template <typename T> auto fmt_arg(const T &v) { if constexpr (std::is_same_v<T, std::string>) return v.c_str(); else return v; }
template <typename... A> void stream_format(std::ostream &s, const char *fmt, const A &... a) { s << string_format(fmt, fmt_arg(a)...); }
struct disasm_interface {
	enum : u32 { SUPPORTED = 0x80000000, STEP_OUT = 0x20000000, STEP_OVER = 0x10000000, STEP_COND = 0x40000000 };
	struct data_buffer { const u8 *d; u32 base, size; u8 r8(offs_t a) const { a -= base; return a < size ? d[a] : 0; } };
	virtual ~disasm_interface() = default;
	virtual u32 opcode_alignment() const = 0;
	virtual offs_t disassemble(std::ostream &, offs_t, const data_buffer &, const data_buffer &) = 0;
};
}
template <typename T> constexpr T BIT(T x, int n) { return (x >> n) & T(1); }
