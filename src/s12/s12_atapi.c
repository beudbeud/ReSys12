// ReSys12: ATAPI CD-ROM (Toshiba XM-6402B) of the System 12 CDXA PCB, on a MAME CHD image. Task file and PACKET
// protocol as MAME src/devices/machine/atapihle.cpp / t10mmc.cpp (BSD-3-Clause); only the commands the CDXA
// firmware sends are answered, others are logged and rejected. GPL-2.0-or-later.
#include "s12_atapi.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>

enum { ST_ERR = 0x01, ST_DRQ = 0x08, ST_DSC = 0x10, ST_DRDY = 0x40, ST_BSY = 0x80 };
enum { IR_COD = 1, IR_IO = 2 };
enum { PH_IDLE, PH_PACKET, PH_DATA_IN, PH_DATA_OUT };
#define FRAME_BYTES 2448 // 2352 + 96 bytes of subcode per CHD unit
#define BUF_MAX 0x20000

static struct {
	chd_file *chd;
	uint32_t frames, hunk_frames, cached_hunk;
	uint8_t *hunk;
	uint8_t error, features, seccount, sector, cyl_lo, cyl_hi, dev, status;
	int phase;
	uint8_t packet[12];
	int packet_pos;
	uint8_t buf[BUF_MAX];
	uint32_t pos, len, chunk_end, limit;
	uint8_t sense_key, asc;
} cd;

static int s_log = -1;
static void alog(const char *fmt, ...)
{
	if (s_log < 0)
		s_log = getenv("RS12_ATAPILOG") != NULL;
	if (!s_log)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int s12_atapi_open(const char *path)
{
	s12_atapi_close();
	if (chd_open(path, CHD_OPEN_READ, NULL, &cd.chd) != CHDERR_NONE) {
		cd.chd = NULL;
		return -1;
	}
	const chd_header *h = chd_get_header(cd.chd);
	cd.hunk_frames = h->hunkbytes / FRAME_BYTES;
	cd.hunk = (uint8_t *)malloc(h->hunkbytes);
	cd.cached_hunk = ~0u;
	cd.frames = 0;
	char meta[256];
	uint32_t len = 0, tag = 0;
	uint8_t flags = 0;
	// ponytail: one track (both CDXA discs are a single MODE2_RAW track without pregap)
	if (chd_get_metadata(cd.chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, &tag, &flags) == CHDERR_NONE) {
		meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = 0;
		const char *f = strstr(meta, "FRAMES:");
		if (f)
			cd.frames = (uint32_t)atoi(f + 7);
	}
	if (!cd.hunk || !cd.frames || !cd.hunk_frames) {
		s12_atapi_close();
		return -1;
	}
	return 0;
}

void s12_atapi_close(void)
{
	if (cd.chd)
		chd_close(cd.chd);
	free(cd.hunk);
	cd.chd = NULL;
	cd.hunk = NULL;
}

static void signature(void)
{
	cd.seccount = cd.sector = 1;
	cd.cyl_lo = 0x14;
	cd.cyl_hi = 0xeb;
}

void s12_atapi_reset(void)
{
	cd.phase = PH_IDLE;
	cd.error = 1;
	cd.status = ST_DRDY | ST_DSC;
	cd.dev = 0;
	cd.sense_key = cd.asc = 0;
	signature();
}

// raw 2352-byte sector, NULL past the end
static const uint8_t *frame(uint32_t lba)
{
	if (!cd.chd || lba >= cd.frames)
		return NULL;
	const uint32_t h = lba / cd.hunk_frames;
	if (h != cd.cached_hunk) {
		if (chd_read(cd.chd, h, cd.hunk) != CHDERR_NONE)
			return NULL;
		cd.cached_hunk = h;
	}
	return cd.hunk + (lba % cd.hunk_frames) * FRAME_BYTES;
}

static void lba_to_msf(uint32_t lba, uint8_t *m)
{
	lba += 150;
	m[0] = 0;
	m[1] = (uint8_t)(lba / (75 * 60));
	m[2] = (uint8_t)(lba / 75 % 60);
	m[3] = (uint8_t)(lba % 75);
}

static void complete(void)
{
	cd.phase = PH_IDLE;
	cd.cyl_lo = cd.cyl_hi = 0; // MAME: the byte count reads 0 after a transfer, the CDXA firmware never rewrites it
	cd.seccount = IR_COD | IR_IO;
	cd.status = ST_DRDY | ST_DSC;
	cd.error = 0;
}

static void fail(uint8_t key, uint8_t asc)
{
	cd.sense_key = key;
	cd.asc = asc;
	cd.phase = PH_IDLE;
	cd.seccount = IR_COD | IR_IO;
	cd.status = ST_DRDY | ST_DSC | ST_ERR;
	cd.error = (uint8_t)(key << 4);
}

// next DRQ block of the data in buf, limited by the byte count the host set
static void next_chunk(void)
{
	// MAME atapi_hle_device::fill_buffer: a block short of the rest is cut to whole 2048-byte sectors
	uint32_t n = cd.len - cd.pos;
	if (cd.limit < n)
		n = cd.limit - cd.limit % 2048;
	if (!n) {
		complete();
		return;
	}
	cd.chunk_end = cd.pos + n;
	alog("atapi drq %u bytes (of %u)\n", n, cd.len - cd.pos);
	cd.cyl_lo = (uint8_t)n;
	cd.cyl_hi = (uint8_t)(n >> 8);
	cd.seccount = IR_IO;
	cd.status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void data_in(uint32_t len)
{
	if (!len) {
		complete();
		return;
	}
	cd.len = len;
	cd.pos = 0;
	cd.phase = PH_DATA_IN;
	cd.limit = (uint32_t)cd.cyl_hi << 8 | cd.cyl_lo; // the host's byte count limit per DRQ block
	if (!cd.limit || cd.limit >= 0xfffe)
		cd.limit = 0xfffe;
	cd.limit &= ~1u;
	next_chunk();
}

static void data_out(uint32_t len)
{
	cd.len = len < BUF_MAX ? len : BUF_MAX;
	cd.pos = 0;
	cd.phase = PH_DATA_OUT;
	cd.seccount = 0;
	cd.status = ST_DRDY | ST_DSC | ST_DRQ;
}

static void put_ata_string(uint8_t *w, const char *s, int bytes)
{
	for (int i = 0; i < bytes; i++)
		w[i ^ 1] = (uint8_t)(i < (int)strlen(s) ? s[i] : ' ');
}

// READ CD byte 9: sync, header codes, user data, EDC/ECC -> one span of the raw sector (mode 2)
static int read_cd_span(uint8_t flags, int form2, int *start)
{
	// form 2 user data is 2328 bytes (2324 + its EDC) and has no separate EDC/ECC field (MAME t10mmc.cpp)
	const int sync = flags & 0x80, hdr = (flags >> 5) & 3, user = flags & 0x10, edc = (flags & 0x08) && !form2;
	int s = sync ? 0 : (hdr & 1) ? 12 : (hdr & 2) ? 16 : user ? 24 : 2352;
	int e = edc ? 2352 : user ? (form2 ? 24 + 2328 : 24 + 2048) : (hdr & 2) ? 24 : (hdr & 1) ? 16 : sync ? 12 : 0;
	if (edc && !user && s > 2072)
		s = 2072;
	*start = s;
	return e > s ? e - s : 0;
}

static void packet(void)
{
	const uint8_t *p = cd.packet;
	const uint32_t lba = (uint32_t)p[2] << 24 | p[3] << 16 | p[4] << 8 | p[5];
	memset(cd.buf, 0, 512);
	if (!cd.chd && p[0] != 0x03 && p[0] != 0x12) { // no disc image
		fail(2, 0x3a);
		return;
	}
	alog("atapi cmd %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3], p[4], p[5],
		p[6], p[7], p[8], p[9], p[10], p[11]);
	switch (p[0]) {
	case 0x00: // TEST UNIT READY
	case 0x1b: // START STOP UNIT
	case 0x1e: // PREVENT ALLOW MEDIUM REMOVAL
	case 0x2b: // SEEK
	case 0x45: case 0x47: case 0x4b: case 0x4e: // PLAY AUDIO, PLAY MSF, PAUSE, STOP (data disc: nothing to play)
	case 0xbb: // SET CD SPEED
		complete();
		return;
	case 0x03: // REQUEST SENSE
		cd.buf[0] = 0x70;
		cd.buf[2] = cd.sense_key;
		cd.buf[7] = 10;
		cd.buf[12] = cd.asc;
		cd.sense_key = cd.asc = 0;
		data_in(p[4] < 18 ? p[4] : 18);
		return;
	case 0x12: // INQUIRY
		cd.buf[0] = 5;
		cd.buf[1] = 0x80;
		cd.buf[3] = 0x21;
		cd.buf[4] = 31;
		memcpy(cd.buf + 8, "TOSHIBA CD-ROM XM-6402B 1008", 28);
		data_in(p[4] < 36 ? p[4] : 36);
		return;
	case 0x25: { // READ CAPACITY
		const uint32_t last = cd.frames - 1;
		cd.buf[0] = (uint8_t)(last >> 24); cd.buf[1] = (uint8_t)(last >> 16); cd.buf[2] = (uint8_t)(last >> 8); cd.buf[3] = (uint8_t)last;
		cd.buf[6] = 0x08;
		data_in(8);
		return;
	}
	case 0x28: case 0xa8: { // READ(10), READ(12): 2048-byte user data
		const uint32_t n = p[0] == 0x28 ? (uint32_t)(p[7] << 8 | p[8]) : (uint32_t)(p[6] << 24 | p[7] << 16 | p[8] << 8 | p[9]);
		if (n * 2048 > BUF_MAX) { fail(5, 0x24); return; }
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *f = frame(lba + i);
			if (!f) { fail(5, 0x21); return; }
			memcpy(cd.buf + i * 2048, f + 24, 2048);
		}
		data_in(n * 2048);
		return;
	}
	case 0xbe: { // READ CD
		const uint32_t n = (uint32_t)(p[6] << 16 | p[7] << 8 | p[8]);
		uint32_t out = 0;
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *f = frame(lba + i);
			if (!f) { fail(5, 0x21); return; }
			int start;
			const int len = read_cd_span(p[9], (f[18] >> 5) & 1, &start);
			if (out + (uint32_t)len > BUF_MAX) { fail(5, 0x24); return; }
			memcpy(cd.buf + out, f + start, (size_t)len);
			out += (uint32_t)len;
		}
		data_in(out);
		return;
	}
	case 0x43: { // READ TOC: track 1 and the lead-out
		const int msf = p[1] & 2;
		uint8_t *b = cd.buf;
		b[1] = 18; b[2] = 1; b[3] = 1;
		b[5] = 0x14; b[6] = 1;
		if (msf) lba_to_msf(0, b + 8);
		b[13] = 0x14; b[14] = 0xaa;
		if (msf) lba_to_msf(cd.frames, b + 16);
		else { b[16] = (uint8_t)(cd.frames >> 24); b[17] = (uint8_t)(cd.frames >> 16); b[18] = (uint8_t)(cd.frames >> 8); b[19] = (uint8_t)cd.frames; }
		const uint32_t alloc = (uint32_t)(p[7] << 8 | p[8]);
		data_in(alloc < 20 ? alloc : 20);
		return;
	}
	case 0x42: { // READ SUB-CHANNEL: no audio play
		cd.buf[1] = 0x15;
		const uint32_t alloc = (uint32_t)(p[7] << 8 | p[8]);
		data_in(alloc < 4 ? alloc : 4);
		return;
	}
	case 0x1a: case 0x5a: { // MODE SENSE(6/10): header and an empty page
		const int page = p[2] & 0x3f, ten = p[0] == 0x5a;
		const int hdr = ten ? 8 : 4, plen = page == 0x2a ? 20 : page == 0x0e ? 16 : 8;
		uint8_t *b = cd.buf;
		b[hdr] = (uint8_t)page;
		b[hdr + 1] = (uint8_t)(plen - 2);
		const int total = hdr + plen;
		if (ten) b[1] = (uint8_t)(total - 2); else b[0] = (uint8_t)(total - 1);
		const uint32_t alloc = ten ? (uint32_t)(p[7] << 8 | p[8]) : p[4];
		data_in(alloc < (uint32_t)total ? alloc : (uint32_t)total);
		return;
	}
	case 0x15: case 0x55: // MODE SELECT(6/10): the parameters are taken and ignored
		data_out(p[0] == 0x15 ? p[4] : (uint32_t)(p[7] << 8 | p[8]));
		if (!cd.len)
			complete();
		return;
	}
	alog("atapi: unsupported command %02x\n", p[0]);
	fail(5, 0x20);
}

static void command(uint8_t c)
{
	alog("atapi ata %02x (bytes %04x features %02x)\n", c, cd.cyl_hi << 8 | cd.cyl_lo, cd.features);
	switch (c) {
	case 0xa0: // PACKET
		cd.phase = PH_PACKET;
		cd.packet_pos = 0;
		cd.seccount = IR_COD;
		cd.status = ST_DRDY | ST_DSC | ST_DRQ;
		return;
	case 0xa1: { // IDENTIFY PACKET DEVICE
		memset(cd.buf, 0, 512);
		// as MAME atapi_cdrom_device + the CDXA drive name: no DMA (word 49), microprocessor DRQ (word 0)
		cd.buf[0] = 0x80; cd.buf[1] = 0x85;
		put_ata_string(cd.buf + 20, "", 20);
		put_ata_string(cd.buf + 46, "1.0", 8);
		put_ata_string(cd.buf + 54, "TOSHIBA CD-ROM XM-6402B", 40);
		cd.buf[99] = 0x06;
		cd.len = 512;
		cd.pos = 0;
		cd.chunk_end = 512;
		cd.phase = PH_DATA_IN;
		cd.seccount = IR_IO;
		cd.status = ST_DRDY | ST_DSC | ST_DRQ;
		return;
	}
	case 0x08: // DEVICE RESET
	case 0x90: // EXECUTE DEVICE DIAGNOSTIC
		s12_atapi_reset();
		return;
	case 0xec: // IDENTIFY DEVICE: aborted with the packet signature
		signature();
		cd.error = 4;
		cd.status = ST_DRDY | ST_ERR;
		return;
	case 0xe5: // CHECK POWER MODE: active or idle
		cd.seccount = 0xff;
		cd.status = ST_DRDY | ST_DSC;
		return;
	case 0xef: // SET FEATURES
	case 0x00: // NOP
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: // STANDBY / IDLE (IMMEDIATE)
		cd.status = ST_DRDY | ST_DSC;
		return;
	}
	alog("atapi: unsupported ata command %02x\n", c);
	cd.error = 4;
	cd.status = ST_DRDY | ST_ERR;
}

int s12_atapi_drq(void)
{
	return cd.phase == PH_DATA_IN && cd.pos < cd.chunk_end;
}

uint16_t s12_atapi_r(int reg)
{
	switch (reg) {
	case 0:
		if (cd.phase == PH_DATA_IN && cd.pos < cd.chunk_end) {
			const uint16_t v = (uint16_t)(cd.buf[cd.pos] | cd.buf[cd.pos + 1] << 8);
			cd.pos += 2;
			if (cd.pos >= cd.chunk_end) {
				if (cd.pos >= cd.len) {
					complete();
				} else {
					next_chunk();
				}
			}
			return v;
		}
		return 0;
	case 1: return cd.error;
	case 2: return cd.seccount;
	case 3: return cd.sector;
	case 4: return cd.cyl_lo;
	case 5: return cd.cyl_hi;
	case 6: return cd.dev;
	case 7: return cd.status;
	}
	return 0;
}

void s12_atapi_w(int reg, uint16_t v)
{
	if (reg)
		alog("atapi w%d %04x\n", reg, v);
	switch (reg) {
	case 0:
		if (cd.phase == PH_PACKET) {
			cd.packet[cd.packet_pos] = (uint8_t)v;
			cd.packet[cd.packet_pos + 1] = (uint8_t)(v >> 8);
			cd.packet_pos += 2;
			if (cd.packet_pos >= 12)
				packet();
		} else if (cd.phase == PH_DATA_OUT) {
			cd.pos += 2;
			if (cd.pos >= cd.len)
				complete();
		}
		return;
	case 1: cd.features = (uint8_t)v; return;
	case 2: cd.seccount = (uint8_t)v; return;
	case 3: cd.sector = (uint8_t)v; return;
	case 4: cd.cyl_lo = (uint8_t)v; return;
	case 5: cd.cyl_hi = (uint8_t)v; return;
	case 6: cd.dev = (uint8_t)v; return;
	case 7: command((uint8_t)v); return;
	}
}

uint32_t s12_atapi_state_size(void)
{
	return sizeof(cd);
}

void s12_atapi_state_save(void *dst)
{
	memcpy(dst, &cd, sizeof(cd));
}

void s12_atapi_state_load(const void *src)
{
	chd_file *chd = cd.chd;
	uint8_t *hunk = cd.hunk;
	const uint32_t frames = cd.frames, hunk_frames = cd.hunk_frames;
	memcpy(&cd, src, sizeof(cd));
	cd.chd = chd;
	cd.hunk = hunk;
	cd.frames = frames;
	cd.hunk_frames = hunk_frames;
	cd.cached_hunk = ~0u;
}
