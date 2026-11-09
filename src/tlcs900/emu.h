// ReSys10: the few parts of MAME's device framework the TLCS-900/H core (tlcs900.cpp, 900tbl.hxx, 900htbl.hxx) and the
// TMP95C061 (tmp95c061.cpp) use, so those files build unchanged outside MAME. Device configuration, address maps,
// device callbacks and save_item work the MAME way on a much smaller scale; the debugger, state display and
// disassembler parts are empty. BSD-3-Clause (as the MAME sources it serves).
#ifndef RESYS10_TLCS900_EMU_H
#define RESYS10_TLCS900_EMU_H

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef uint32_t offs_t;

#define ATTR_COLD
#define NAME(x) x, #x
#define FUNC(x) &x, #x

enum { CLEAR_LINE = 0, ASSERT_LINE = 1 };
enum { INPUT_LINE_NMI = 32 };
enum { AS_PROGRAM = 0 };
enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
enum { STATE_GENPC = -1, STATE_GENPCBASE = -2, STATE_GENFLAGS = -3 };

// (MAME's PAIR under another name: pcsx_rearmed has its own PAIR, and LTO links both)
#define PAIR tlcs900_pair
union PAIR // little-endian hosts (LSB_FIRST)
{
	struct { u8 l, h, h2, h3; } b;
	struct { u16 l, h; } w;
	struct { s8 l, h, h2, h3; } sb;
	struct { s16 l, h; } sw;
	u32 d;
	s32 sd;
};

template <typename T> constexpr T BIT(T x, int n) { return (x >> n) & T(1); }
template <typename T> constexpr T BIT(T x, int n, int w) { return (x >> n) & ((T(1) << w) - 1); }

inline std::string string_format(const char *fmt, ...)
{
	char buf[256];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	return buf;
}

typedef const char *device_type;
#define DECLARE_DEVICE_TYPE(Type, Class) extern device_type const Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) device_type const Type = Short;
#define DERIVED_CLOCK(a, b) 0

struct machine_config {};
namespace util { struct disasm_interface { virtual ~disasm_interface() = default; }; }
struct device_state_entry { int index() const { return 0; } };

// state registry: save_item records every variable of the device (its save state, as MAME's save manager)
struct save_registry {
	std::vector<std::pair<void *, size_t>> items;
	size_t size() const { size_t n = 0; for (auto &i : items) n += i.second; return n; }
	void save(uint8_t *p) const { for (auto &i : items) { memcpy(p, i.first, i.second); p += i.second; } }
	void load(const uint8_t *p) const { for (auto &i : items) { memcpy(i.first, p, i.second); p += i.second; } }
};

struct state_entry_stub {
	state_entry_stub &callimport() { return *this; }
	state_entry_stub &formatstr(const char *) { return *this; }
	state_entry_stub &noshow() { return *this; }
};

class device_t;
// the device whose configuration is being built (device_add_mconfig), as MAME resolves FUNC() against it
inline device_t *&config_device() { static device_t *d; return d; }

class device_t
{
public:
	device_t(const machine_config &, device_type, const char *, device_t *, uint32_t clock) : m_clock(clock) {}
	virtual ~device_t() = default;
	uint32_t clock() const { return m_clock; }
	void logerror(const char *fmt, ...)
	{
		if (!getenv("RS10_TLCS_LOG"))
			return;
		va_list ap;
		va_start(ap, fmt);
		vfprintf(stderr, fmt, ap);
		va_end(ap);
	}
	template <typename T> void save_item(T &v, const char *) { m_save.items.emplace_back((void *)&v, sizeof(v)); }
	save_registry m_save;

	virtual void device_config_complete() {}
	virtual void device_resolve_objects() {}
	virtual void device_start() {}
	virtual void device_reset() {}
	virtual void device_add_mconfig(machine_config &) {}

private:
	uint32_t m_clock;
};

// ---- devcb: callbacks bound with bind().set(...) ----
template <typename T> struct devcb_read
{
	std::function<T(offs_t)> f;
	T def;
	template <size_t N> struct array {
		devcb_read e[N];
		array(device_t &d, T v) { for (auto &x : e) x.def = v; }
		devcb_read &operator[](size_t i) { return e[i]; }
	};
	devcb_read() : def(0) {}
	devcb_read(device_t &, T v) : def(v) {}
	struct binder { devcb_read *p; template <typename F> void set(F fn) { p->set(fn); } };
	binder bind() { return binder{ this }; } // (by value: the MAME helpers return it as auto)
	template <typename F> void set(F fn)
	{
		if constexpr (std::is_invocable_v<F, offs_t>)
			f = fn;
		else
			f = [fn] (offs_t) { return fn(); };
	}
	T operator()(offs_t o = 0) { return f ? f(o) : def; }
};
typedef devcb_read<u8> devcb_read8;
typedef devcb_read<u16> devcb_read16;

struct devcb_write8
{
	std::function<void(offs_t, u8)> f;
	template <size_t N> struct array {
		devcb_write8 e[N];
		array(device_t &) {}
		devcb_write8 &operator[](size_t i) { return e[i]; }
	};
	devcb_write8() {}
	devcb_write8(device_t &) {}
	struct binder {
		devcb_write8 *p;
		template <typename F> void set(F fn) { p->set(fn); }
		template <typename C> void set(void (C::*m)(u8), const char *n) { p->set(m, n); }
	};
	binder bind() { return binder{ this }; } // (by value: the MAME helpers return it as auto)
	template <typename F> void set(F fn)
	{
		if constexpr (std::is_invocable_v<F, offs_t, u8>)
			f = fn;
		else
			f = [fn] (offs_t, u8 d) { fn(d); };
	}
	// FUNC(owner::member): a member of the device being configured
	template <typename C> void set(void (C::*m)(u8), const char *)
	{
		C *c = static_cast<C *>(config_device());
		f = [c, m] (offs_t, u8 d) { (c->*m)(d); };
	}
	void operator()(offs_t o, u8 d, u8 = 0xff) { if (f) f(o, d); }
	void operator()(u8 d) { if (f) f(0, d); }
};

// ---- address maps: the TMP95C061's internal registers (8-bit handlers) ----
struct address_map_entry
{
	offs_t start = 0, end = 0;
	std::function<u8(offs_t)> rd;
	std::function<void(offs_t, u8)> wr;
	device_t *dev = nullptr;

	template <typename C> address_map_entry &r(u8 (C::*m)(), const char *)
	{ C *c = static_cast<C *>(dev); rd = [c, m] (offs_t) { return (c->*m)(); }; return *this; }
	template <typename C> address_map_entry &r(u8 (C::*m)(offs_t), const char *)
	{ C *c = static_cast<C *>(dev); rd = [c, m] (offs_t o) { return (c->*m)(o); }; return *this; }
	template <typename C> address_map_entry &w(void (C::*m)(u8), const char *)
	{ C *c = static_cast<C *>(dev); wr = [c, m] (offs_t, u8 d) { (c->*m)(d); }; return *this; }
	template <typename C> address_map_entry &w(void (C::*m)(offs_t, u8), const char *)
	{ C *c = static_cast<C *>(dev); wr = [c, m] (offs_t o, u8 d) { (c->*m)(o, d); }; return *this; }
	template <typename R, typename W> address_map_entry &rw(R r_, const char *rn, W w_, const char *wn)
	{ r(r_, rn); return w(w_, wn); }
};

struct address_map
{
	device_t *dev;
	std::vector<address_map_entry> entries;
	explicit address_map(device_t *d) : dev(d) {}
	address_map_entry &operator()(offs_t s, offs_t e)
	{
		entries.emplace_back();
		entries.back().start = s;
		entries.back().end = e;
		entries.back().dev = dev;
		return entries.back();
	}
};

struct address_map_constructor
{
	std::function<void(address_map &)> f;
	address_map_constructor() {}
	template <typename C> address_map_constructor(void (C::*m)(address_map &), const char *, C *c)
		: f([c, m] (address_map &map) { (c->*m)(map); }) {}
};

struct address_space_config
{
	address_map_constructor internal;
	address_space_config() {}
	address_space_config(const char *, endianness_t, int, int, int, address_map_constructor m) : internal(std::move(m)) {}
};

// 24-bit little-endian space: the internal registers (0x00-0x7f) first, the rest from the board (ext_r / ext_w)
class address_space
{
public:
	std::function<u8(offs_t)> ext_r;
	std::function<void(offs_t, u8)> ext_w;
	// optional: aligned 16-bit accesses outside the internal registers (a board's 16-bit devices see one access)
	std::function<u16(offs_t)> ext_r16;
	bool trace_io = false; // diagnostics
	// RAM the board maps straight into the space (4KB pages, little-endian bytes); null = the ext_* callbacks
	u8 *page[0x1000] = {};
	// pages holding decoded code (a block cache): their writes call code_hook with the written host byte
	u8 code_page[0x1000] = {};
	void (*code_hook)(void *ctx, const u8 *host) = nullptr;
	void *code_ctx = nullptr;
	void map_ram(offs_t start, offs_t end, u8 *mem) // (start, end + 1 on 4KB boundaries)
	{
		for (offs_t a = start; a <= end; a += 0x1000)
			page[(a & 0xffffff) >> 12] = mem + (a - start);
	}
	std::function<void(offs_t, u16)> ext_w16;
	address_map_entry *io[0x80] = {};
	std::vector<address_map_entry> entries;

	void install(const address_space_config &cfg, device_t *dev)
	{
		address_map map(dev);
		if (cfg.internal.f)
			cfg.internal.f(map);
		entries = std::move(map.entries);
		std::fill(std::begin(io), std::end(io), nullptr);
		for (auto &e : entries)
			for (offs_t a = e.start; a <= e.end && a < 0x80; a++)
				io[a] = &e;
	}
	u8 read_byte(offs_t a)
	{
		a &= 0xffffff;
		if (u8 *p = page[a >> 12])
			return p[a & 0xfff];
		if (a < 0x80) {
			address_map_entry *e = io[a];
			if (trace_io)
				fprintf(stderr, "tlcs io r %02x\n", a);
			return e && e->rd ? e->rd(a - e->start) : 0;
		}
		return ext_r ? ext_r(a) : 0;
	}
	void write_byte(offs_t a, u8 d)
	{
		a &= 0xffffff;
		if (u8 *p = page[a >> 12]) {
			p[a & 0xfff] = d;
			if (code_page[a >> 12])
				code_hook(code_ctx, p + (a & 0xfff));
			return;
		}
		if (a < 0x80) {
			address_map_entry *e = io[a];
			if (e && e->wr)
				e->wr(a - e->start, d);
			return;
		}
		if (ext_w)
			ext_w(a, d);
	}
	u16 read_word_unaligned(offs_t a)
	{
		a &= 0xffffff;
		if (u8 *p = page[a >> 12]; p && (a & 0xfff) != 0xfff)
			return u16(p[a & 0xfff] | (p[(a & 0xfff) + 1] << 8));
		if (ext_r16 && !(a & 1) && a >= 0x80)
			return ext_r16(a);
		return u16(read_byte(a) | (read_byte(a + 1) << 8));
	}
	u32 read_dword_unaligned(offs_t a) { return read_word_unaligned(a) | (u32(read_word_unaligned(a + 2)) << 16); }
	void write_word_unaligned(offs_t a, u16 d)
	{
		a &= 0xffffff;
		if (u8 *p = page[a >> 12]; p && (a & 0xfff) != 0xfff) {
			p[a & 0xfff] = u8(d);
			p[(a & 0xfff) + 1] = u8(d >> 8);
			if (code_page[a >> 12])
				code_hook(code_ctx, p + (a & 0xfff));
			return;
		}
		if (ext_w16 && !(a & 1) && a >= 0x80) {
			ext_w16(a, d);
			return;
		}
		write_byte(a, u8(d));
		write_byte(a + 1, u8(d >> 8));
	}
	void write_dword_unaligned(offs_t a, u32 d) { write_word_unaligned(a, u16(d)); write_word_unaligned(a + 2, u16(d >> 16)); }
};

class device_memory_interface
{
public:
	using space_config_vector = std::vector<std::pair<int, const address_space_config *>>;
};

class cpu_device : public device_t, public device_memory_interface
{
public:
	cpu_device(const machine_config &mc, device_type t, const char *tag, device_t *owner, uint32_t clock)
		: device_t(mc, t, tag, owner, clock) {}
	address_space &space(int = AS_PROGRAM) { return m_space; }
	address_space m_space;
	int *m_icountptr = nullptr;

	virtual uint32_t execute_min_cycles() const noexcept { return 1; }
	virtual uint32_t execute_max_cycles() const noexcept { return 1; }
	virtual bool execute_input_edge_triggered(int) const noexcept { return false; }
	virtual void execute_run() {}
	virtual void execute_set_input(int, int) {}
	virtual space_config_vector memory_space_config() const { return {}; }
	virtual void state_import(const device_state_entry &) {}
	virtual void state_string_export(const device_state_entry &, std::string &) const {}
	virtual std::unique_ptr<util::disasm_interface> create_disassembler() { return nullptr; }

protected:
	void set_icountptr(int &c) { m_icountptr = &c; }
	template <typename T> state_entry_stub state_add(int, const char *, T &) { return {}; }
	void debugger_instruction_hook(offs_t) {}
	void debugger_wait_hook() {}
};

// devices made by a configuration (the TMP95C061's serial channels): owned here, configured in place
template <typename T, size_t N> struct required_device_array
{
	std::unique_ptr<T> d[N];
	required_device_array(device_t &, const char *, unsigned)
	{
		for (auto &x : d)
			x = std::make_unique<T>(machine_config(), "", nullptr, 0);
	}
	struct ref {
		T *p;
		T *operator->() const { return p; }
		T *lookup() const { return p; }
		operator T *() const { return p; }
	};
	ref operator[](size_t i) { return ref{ d[i].get() }; }
};

#endif
