// ReSys12: Hitachi SH-2 interpreter. GPL-2.0-or-later. See sh2.h.
#include "sh2.h"

#define RN s->r[(op >> 8) & 15]
#define RM s->r[(op >> 4) & 15]
#define R0 s->r[0]
#define T (s->sr & SH2_T)
#define SET_T(c) (s->sr = (c) ? (s->sr | SH2_T) : (s->sr & ~SH2_T))
#define SR_MASK 0x3f3u

static inline uint32_t sx8(uint32_t v) { return (uint32_t)(int32_t)(int8_t)v; }
static inline uint32_t sx16(uint32_t v) { return (uint32_t)(int32_t)(int16_t)v; }

void sh2_reset(SH2 *s)
{
	SH2Bus bus = s->bus;
	void (*ack)(int) = s->irq_ack;
	for (int i = 0; i < 16; i++)
		s->r[i] = 0;
	s->pr = s->gbr = s->mach = s->macl = 0;
	s->vbr = 0;
	s->sr = 0xf0; // I = 15
	s->irq_level = 0;
	s->sleeping = 0;
	s->bus = bus;
	s->irq_ack = ack;
	s->pc = bus.rd32(0);
	s->r[15] = bus.rd32(4);
}

static void push(SH2 *s, uint32_t v)
{
	s->r[15] -= 4;
	s->bus.wr32(s->r[15], v);
}

static void exception(SH2 *s, int vector, uint32_t ret_pc)
{
	push(s, s->sr);
	push(s, ret_pc);
	s->pc = s->bus.rd32(s->vbr + (uint32_t)vector * 4);
}

int sh2_check_irq(SH2 *s)
{
	if (!s->irq_level || s->irq_level <= (int)((s->sr >> 4) & 15))
		return 0;
	const int vector = s->irq_vector, level = s->irq_level;
	exception(s, vector, s->pc);
	s->sr = (s->sr & ~0xf0u) | ((uint32_t)level << 4);
	if (s->irq_ack)
		s->irq_ack(vector);
	s->sleeping = 0;
	s->cycles += 13;
	return 1;
}

static void exec(SH2 *s, uint16_t op);

// delayed branch: the slot runs at its own address, then the jump
static void delay(SH2 *s, uint32_t target)
{
	s->pc += 2;
	exec(s, s->bus.rd16(s->pc));
	s->pc = target;
	s->cycles++;
}

static void div1(SH2 *s, uint32_t *rn, uint32_t rm)
{
	const int old_q = (s->sr & SH2_Q) != 0, m = (s->sr & SH2_M) != 0;
	int q = (*rn & 0x80000000u) != 0;
	*rn = (*rn << 1) | (s->sr & SH2_T);
	const uint32_t tmp0 = *rn;
	int tmp1;
	if (old_q == m) {
		*rn -= rm;
		tmp1 = *rn > tmp0;
	} else {
		*rn += rm;
		tmp1 = *rn < tmp0;
	}
	q ^= tmp1 ^ m; // the manual's four Q tables in one: shifted-out bit ^ carry/borrow ^ M
	s->sr = q ? (s->sr | SH2_Q) : (s->sr & ~SH2_Q);
	SET_T(q == m);
}

static void exec(SH2 *s, uint16_t op)
{
	const uint32_t pc = s->pc;
	uint32_t *rn = &RN, *rm = &RM;
	const uint32_t d4 = op & 15, d8 = op & 0xff;
	s->cycles++;
	switch (op >> 12) {
	case 0x0:
		switch (op & 0xf) {
		case 0x2:
			switch ((op >> 4) & 15) {
			case 0: *rn = s->sr; break;
			case 1: *rn = s->gbr; break;
			case 2: *rn = s->vbr; break;
			}
			break;
		case 0x3:
			if (((op >> 4) & 15) == 0) { // BSRF
				s->pr = pc + 4;
				delay(s, pc + 4 + *rn);
				return;
			}
			if (((op >> 4) & 15) == 2) { // BRAF
				delay(s, pc + 4 + *rn);
				return;
			}
			break;
		case 0x4: s->bus.wr8(*rn + R0, (uint8_t)*rm); break;
		case 0x5: s->bus.wr16(*rn + R0, (uint16_t)*rm); break;
		case 0x6: s->bus.wr32(*rn + R0, *rm); break;
		case 0x7: s->macl = *rn * *rm; s->cycles++; break;
		case 0x8:
			switch ((op >> 4) & 15) {
			case 0: s->sr &= ~SH2_T; break;
			case 1: s->sr |= SH2_T; break;
			case 2: s->mach = s->macl = 0; break;
			}
			break;
		case 0x9:
			switch ((op >> 4) & 15) {
			case 0: break;
			case 1: s->sr &= ~(SH2_M | SH2_Q | SH2_T); break;
			case 2: *rn = T; break;
			}
			break;
		case 0xa:
			switch ((op >> 4) & 15) {
			case 0: *rn = s->mach; break;
			case 1: *rn = s->macl; break;
			case 2: *rn = s->pr; break;
			}
			break;
		case 0xb:
			switch ((op >> 4) & 15) {
			case 0: delay(s, s->pr); return; // RTS
			case 1: s->sleeping = 1; s->pc = pc + 2; return; // SLEEP: waits for an interrupt, which returns after it
			case 2: { // RTE
				const uint32_t ret = s->bus.rd32(s->r[15]);
				s->r[15] += 4;
				s->sr = s->bus.rd32(s->r[15]) & SR_MASK;
				s->r[15] += 4;
				s->cycles += 3;
				delay(s, ret);
				return;
			}
			}
			break;
		case 0xc: *rn = sx8(s->bus.rd8(*rm + R0)); break;
		case 0xd: *rn = sx16(s->bus.rd16(*rm + R0)); break;
		case 0xe: *rn = s->bus.rd32(*rm + R0); break;
		case 0xf: { // MAC.L @Rm+,@Rn+
			const int32_t a = (int32_t)s->bus.rd32(*rn);
			*rn += 4;
			const int32_t b = (int32_t)s->bus.rd32(*rm);
			*rm += 4;
			int64_t sum = (int64_t)(((uint64_t)s->mach << 32) | s->macl) + (int64_t)a * b;
			if (s->sr & SH2_S) {
				if (sum > 0x00007fffffffffffLL) sum = 0x00007fffffffffffLL;
				if (sum < -0x0000800000000000LL) sum = -0x0000800000000000LL;
			}
			s->mach = (uint32_t)((uint64_t)sum >> 32);
			s->macl = (uint32_t)sum;
			s->cycles += 2;
			break;
		}
		}
		break;
	case 0x1: s->bus.wr32(*rn + d4 * 4, *rm); break;
	case 0x2:
		switch (op & 0xf) {
		case 0x0: s->bus.wr8(*rn, (uint8_t)*rm); break;
		case 0x1: s->bus.wr16(*rn, (uint16_t)*rm); break;
		case 0x2: s->bus.wr32(*rn, *rm); break;
		case 0x4: { const uint32_t v = *rm; *rn -= 1; s->bus.wr8(*rn, (uint8_t)v); break; }
		case 0x5: { const uint32_t v = *rm; *rn -= 2; s->bus.wr16(*rn, (uint16_t)v); break; }
		case 0x6: { const uint32_t v = *rm; *rn -= 4; s->bus.wr32(*rn, v); break; }
		case 0x7: { // DIV0S
			const int q = (*rn >> 31) & 1, m = (*rm >> 31) & 1;
			s->sr = (s->sr & ~(SH2_Q | SH2_M)) | (q ? SH2_Q : 0) | (m ? SH2_M : 0);
			SET_T(q != m);
			break;
		}
		case 0x8: SET_T((*rn & *rm) == 0); break;
		case 0x9: *rn &= *rm; break;
		case 0xa: *rn ^= *rm; break;
		case 0xb: *rn |= *rm; break;
		case 0xc: { const uint32_t x = *rn ^ *rm;
			SET_T(!(x & 0xff000000u) || !(x & 0xff0000u) || !(x & 0xff00u) || !(x & 0xffu)); break; }
		case 0xd: *rn = (*rn >> 16) | (*rm << 16); break;
		case 0xe: s->macl = (*rn & 0xffff) * (*rm & 0xffff); break;
		case 0xf: s->macl = (uint32_t)((int32_t)(int16_t)*rn * (int32_t)(int16_t)*rm); break;
		}
		break;
	case 0x3:
		switch (op & 0xf) {
		case 0x0: SET_T(*rn == *rm); break;
		case 0x2: SET_T(*rn >= *rm); break;
		case 0x3: SET_T((int32_t)*rn >= (int32_t)*rm); break;
		case 0x4: div1(s, rn, *rm); break;
		case 0x5: { const uint64_t r = (uint64_t)*rn * *rm; s->mach = (uint32_t)(r >> 32); s->macl = (uint32_t)r; s->cycles++; break; }
		case 0x6: SET_T(*rn > *rm); break;
		case 0x7: SET_T((int32_t)*rn > (int32_t)*rm); break;
		case 0x8: *rn -= *rm; break;
		case 0xa: { const uint32_t a = *rn, b = *rm, t = T, r = a - b - t;
			SET_T(a < b || (a - b) < t); *rn = r; break; }
		case 0xb: { const int32_t a = (int32_t)*rn, b = (int32_t)*rm; const int32_t r = (int32_t)((uint32_t)a - (uint32_t)b);
			SET_T(((a ^ b) & (a ^ r)) < 0); *rn = (uint32_t)r; break; }
		case 0xc: *rn += *rm; break;
		case 0xd: { const int64_t r = (int64_t)(int32_t)*rn * (int32_t)*rm; s->mach = (uint32_t)((uint64_t)r >> 32); s->macl = (uint32_t)r; s->cycles++; break; }
		case 0xe: { const uint32_t a = *rn, b = *rm, t = T, s1 = a + b, r = s1 + t;
			SET_T(s1 < a || r < s1); *rn = r; break; }
		case 0xf: { const int32_t a = (int32_t)*rn, b = (int32_t)*rm; const int32_t r = (int32_t)((uint32_t)a + (uint32_t)b);
			SET_T((~(a ^ b) & (a ^ r)) < 0); *rn = (uint32_t)r; break; }
		}
		break;
	case 0x4:
		if ((op & 0xf) == 0xf) { // MAC.W @Rm+,@Rn+
			const int32_t a = (int16_t)s->bus.rd16(*rn);
			*rn += 2;
			const int32_t b = (int16_t)s->bus.rd16(*rm);
			*rm += 2;
			const int64_t p = (int64_t)a * b;
			if (s->sr & SH2_S) {
				int64_t sum = (int64_t)(int32_t)s->macl + p;
				if (sum > 0x7fffffffLL) { sum = 0x7fffffffLL; s->mach |= 1; }
				if (sum < -0x80000000LL) { sum = -0x80000000LL; s->mach |= 1; }
				s->macl = (uint32_t)sum;
			} else {
				const int64_t sum = (int64_t)(((uint64_t)s->mach << 32) | s->macl) + p;
				s->mach = (uint32_t)((uint64_t)sum >> 32);
				s->macl = (uint32_t)sum;
			}
			s->cycles += 2;
			break;
		}
		switch (op & 0xff) {
		case 0x00: SET_T(*rn >> 31); *rn <<= 1; break;
		case 0x01: SET_T(*rn & 1); *rn >>= 1; break;
		case 0x02: *rn -= 4; s->bus.wr32(*rn, s->mach); break;
		case 0x03: *rn -= 4; s->bus.wr32(*rn, s->sr); s->cycles++; break;
		case 0x04: SET_T(*rn >> 31); *rn = (*rn << 1) | (*rn >> 31); break;
		case 0x05: SET_T(*rn & 1); *rn = (*rn >> 1) | (*rn << 31); break;
		case 0x06: s->mach = s->bus.rd32(*rn); *rn += 4; break;
		case 0x07: s->sr = s->bus.rd32(*rn) & SR_MASK; *rn += 4; s->cycles += 2; break;
		case 0x08: *rn <<= 2; break;
		case 0x09: *rn >>= 2; break;
		case 0x0a: s->mach = *rn; break;
		case 0x0b: s->pr = pc + 4; delay(s, *rn); return; // JSR
		case 0x0e: s->sr = *rn & SR_MASK; break;
		case 0x10: *rn -= 1; SET_T(*rn == 0); break;
		case 0x11: SET_T((int32_t)*rn >= 0); break;
		case 0x12: *rn -= 4; s->bus.wr32(*rn, s->macl); break;
		case 0x13: *rn -= 4; s->bus.wr32(*rn, s->gbr); s->cycles++; break;
		case 0x15: SET_T((int32_t)*rn > 0); break;
		case 0x16: s->macl = s->bus.rd32(*rn); *rn += 4; break;
		case 0x17: s->gbr = s->bus.rd32(*rn); *rn += 4; s->cycles += 2; break;
		case 0x18: *rn <<= 8; break;
		case 0x19: *rn >>= 8; break;
		case 0x1a: s->macl = *rn; break;
		case 0x1b: { const uint8_t v = s->bus.rd8(*rn); SET_T(v == 0); s->bus.wr8(*rn, v | 0x80); s->cycles += 3; break; }
		case 0x1e: s->gbr = *rn; break;
		case 0x20: SET_T(*rn >> 31); *rn <<= 1; break;
		case 0x21: SET_T(*rn & 1); *rn = (uint32_t)((int32_t)*rn >> 1); break;
		case 0x22: *rn -= 4; s->bus.wr32(*rn, s->pr); break;
		case 0x23: *rn -= 4; s->bus.wr32(*rn, s->vbr); s->cycles++; break;
		case 0x24: { const uint32_t t = T; SET_T(*rn >> 31); *rn = (*rn << 1) | t; break; }
		case 0x25: { const uint32_t t = T; SET_T(*rn & 1); *rn = (*rn >> 1) | (t << 31); break; }
		case 0x26: s->pr = s->bus.rd32(*rn); *rn += 4; break;
		case 0x27: s->vbr = s->bus.rd32(*rn); *rn += 4; s->cycles += 2; break;
		case 0x28: *rn <<= 16; break;
		case 0x29: *rn >>= 16; break;
		case 0x2a: s->pr = *rn; break;
		case 0x2b: delay(s, *rn); return; // JMP
		case 0x2e: s->vbr = *rn; break;
		}
		break;
	case 0x5: *rn = s->bus.rd32(*rm + d4 * 4); break;
	case 0x6:
		switch (op & 0xf) {
		case 0x0: *rn = sx8(s->bus.rd8(*rm)); break;
		case 0x1: *rn = sx16(s->bus.rd16(*rm)); break;
		case 0x2: *rn = s->bus.rd32(*rm); break;
		case 0x3: *rn = *rm; break;
		case 0x4: { const uint32_t a = *rm; if (rn != rm) *rm += 1; *rn = sx8(s->bus.rd8(a)); break; }
		case 0x5: { const uint32_t a = *rm; if (rn != rm) *rm += 2; *rn = sx16(s->bus.rd16(a)); break; }
		case 0x6: { const uint32_t a = *rm; if (rn != rm) *rm += 4; *rn = s->bus.rd32(a); break; }
		case 0x7: *rn = ~*rm; break;
		case 0x8: *rn = (*rm & 0xffff0000u) | ((*rm & 0xff) << 8) | ((*rm >> 8) & 0xff); break;
		case 0x9: *rn = (*rm << 16) | (*rm >> 16); break;
		case 0xa: { const uint32_t t = T, v = *rm, r = 0 - v - t; SET_T(v != 0 || t); *rn = r; break; }
		case 0xb: *rn = 0 - *rm; break;
		case 0xc: *rn = *rm & 0xff; break;
		case 0xd: *rn = *rm & 0xffff; break;
		case 0xe: *rn = sx8(*rm); break;
		case 0xf: *rn = sx16(*rm); break;
		}
		break;
	case 0x7: *rn += sx8(d8); break;
	case 0x8:
		switch ((op >> 8) & 15) {
		case 0x0: s->bus.wr8(*rm + d4, (uint8_t)R0); break;
		case 0x1: s->bus.wr16(*rm + d4 * 2, (uint16_t)R0); break;
		case 0x4: R0 = sx8(s->bus.rd8(*rm + d4)); break;
		case 0x5: R0 = sx16(s->bus.rd16(*rm + d4 * 2)); break;
		case 0x8: SET_T(R0 == sx8(d8)); break;
		case 0x9: if (T) { s->pc = pc + 4 + sx8(d8) * 2; s->cycles += 2; return; } break;
		case 0xb: if (!T) { s->pc = pc + 4 + sx8(d8) * 2; s->cycles += 2; return; } break;
		case 0xd: if (T) { delay(s, pc + 4 + sx8(d8) * 2); return; } break;
		case 0xf: if (!T) { delay(s, pc + 4 + sx8(d8) * 2); return; } break;
		}
		break;
	case 0x9: *rn = sx16(s->bus.rd16(pc + 4 + d8 * 2)); break;
	case 0xa: delay(s, pc + 4 + (uint32_t)((int32_t)((uint32_t)op << 20) >> 19)); return;
	case 0xb: s->pr = pc + 4; delay(s, pc + 4 + (uint32_t)((int32_t)((uint32_t)op << 20) >> 19)); return;
	case 0xc:
		switch ((op >> 8) & 15) {
		case 0x0: s->bus.wr8(s->gbr + d8, (uint8_t)R0); break;
		case 0x1: s->bus.wr16(s->gbr + d8 * 2, (uint16_t)R0); break;
		case 0x2: s->bus.wr32(s->gbr + d8 * 4, R0); break;
		case 0x3: exception(s, (int)d8, pc + 2); s->cycles += 7; return; // TRAPA
		case 0x4: R0 = sx8(s->bus.rd8(s->gbr + d8)); break;
		case 0x5: R0 = sx16(s->bus.rd16(s->gbr + d8 * 2)); break;
		case 0x6: R0 = s->bus.rd32(s->gbr + d8 * 4); break;
		case 0x7: R0 = ((pc + 4) & ~3u) + d8 * 4; break;
		case 0x8: SET_T((R0 & d8) == 0); break;
		case 0x9: R0 &= d8; break;
		case 0xa: R0 ^= d8; break;
		case 0xb: R0 |= d8; break;
		case 0xc: SET_T((s->bus.rd8(s->gbr + R0) & d8) == 0); s->cycles += 2; break;
		case 0xd: s->bus.wr8(s->gbr + R0, s->bus.rd8(s->gbr + R0) & d8); s->cycles += 2; break;
		case 0xe: s->bus.wr8(s->gbr + R0, s->bus.rd8(s->gbr + R0) ^ d8); s->cycles += 2; break;
		case 0xf: s->bus.wr8(s->gbr + R0, s->bus.rd8(s->gbr + R0) | d8); s->cycles += 2; break;
		}
		break;
	case 0xd: *rn = s->bus.rd32(((pc + 4) & ~3u) + d8 * 4); break;
	case 0xe: *rn = sx8(d8); break;
	case 0xf: break; // ponytail: illegal instruction ignored (general illegal instruction exception not modelled)
	}
	s->pc = pc + 2;
}

void sh2_run(SH2 *s, int64_t target)
{
	while (s->cycles < target) {
		sh2_check_irq(s);
		if (s->sleeping) {
			s->cycles = target;
			return;
		}
		exec(s, s->bus.rd16(s->pc));
	}
}
