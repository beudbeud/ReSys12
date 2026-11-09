// ReSys10: the Taiko no Tatsujin sets' ATAPI CD-ROM, a Matsushita CR-589 (MAME namcos10.cpp, bus/ata/cr589.cpp) on a
// MAME CHD image. Task file, PACKET protocol, INTRQ and DMARQ as MAME src/devices/machine/atahle.cpp, bus/ata/
// atapihle.cpp and t10mmc.cpp (BSD-3-Clause), from ReSys12's s12_atapi.c; commands are answered at once (no busy
// time). Only the commands a game sends are answered, others are logged (RS10_ATAPILOG) and rejected.
#include "s10_atapi.h"

#include <stdarg.h>
#include <stddef.h>
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
	uint32_t frames, hunk_frames, cached_hunk, data_offset; // (data_offset: the 2048 user bytes in a CHD unit)
	uint8_t *hunk;
	// (saved in states from here)
	uint8_t error, features, seccount, sector, cyl_lo, cyl_hi, dev, status, irq, dma;
	int phase;
	uint8_t packet[12];
	int packet_pos;
	uint8_t buf[BUF_MAX];
	uint32_t pos, len, chunk_end, limit;
	uint8_t sense_key, asc;
} cd;
#define CD_STATE_START offsetof(__typeof__(cd), error)

static int s_log = -1;
static void alog(const char *fmt, ...)
{
	if (s_log < 0)
		s_log = getenv("RS10_ATAPILOG") != NULL;
	if (!s_log)
		return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

int s10_atapi_open(const char *path)
{
	s10_atapi_close();
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
	// ponytail: one track (every Taiko disc is a single data track without pregap)
	if (chd_get_metadata(cd.chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof(meta) - 1, &len, &tag, &flags) == CHDERR_NONE) {
		meta[len < sizeof(meta) ? len : sizeof(meta) - 1] = 0;
		const char *f = strstr(meta, "FRAMES:");
		if (f)
			cd.frames = (uint32_t)atoi(f + 7);
		// MODE1_RAW: sync + header (16 bytes) first; MODE2_RAW: + the subheader (24); MODE1 / MODE2_FORM1: the data alone
		cd.data_offset = strstr(meta, "MODE1_RAW") ? 16 : strstr(meta, "MODE2_RAW") ? 24 : 0;
	}
	if (!cd.hunk || !cd.frames || !cd.hunk_frames) {
		s10_atapi_close();
		return -1;
	}
	return 0;
}

void s10_atapi_close(void)
{
	if (cd.chd)
		chd_close(cd.chd);
	free(cd.hunk);
	cd.chd = NULL;
	cd.hunk = NULL;
}

int s10_atapi_present(void) { return cd.chd != NULL; }

static int selected(void) { return !(cd.dev & 0x10); } // (the drive is device 0, alone on the port)

static void set_irq(int state)
{
	if (cd.irq != state) {
		cd.irq = (uint8_t)state;
		s10_atapi_irq(state);
	}
}

static void signature(void)
{
	cd.seccount = cd.sector = 1;
	cd.cyl_lo = 0x14;
	cd.cyl_hi = 0xeb;
}

void s10_atapi_reset(void)
{
	cd.phase = PH_IDLE;
	cd.error = 1;
	cd.status = ST_DRDY | ST_DSC;
	cd.dev = 0;
	cd.dma = 0;
	cd.sense_key = cd.asc = 0;
	signature();
	set_irq(0);
}

// the 2048 user bytes of a sector, NULL past the end
static const uint8_t *sector_data(uint32_t lba)
{
	if (!cd.chd || lba >= cd.frames)
		return NULL;
	const uint32_t h = lba / cd.hunk_frames;
	if (h != cd.cached_hunk) {
		if (chd_read(cd.chd, h, cd.hunk) != CHDERR_NONE)
			return NULL;
		cd.cached_hunk = h;
	}
	return cd.hunk + (lba % cd.hunk_frames) * FRAME_BYTES + cd.data_offset;
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
	cd.cyl_lo = cd.cyl_hi = 0;
	cd.seccount = IR_COD | IR_IO;
	cd.status = ST_DRDY | ST_DSC;
	cd.error = 0;
	set_irq(1);
}

static void fail(uint8_t key, uint8_t asc)
{
	cd.sense_key = key;
	cd.asc = asc;
	cd.phase = PH_IDLE;
	cd.seccount = IR_COD | IR_IO;
	cd.status = ST_DRDY | ST_DSC | ST_ERR;
	cd.error = (uint8_t)(key << 4);
	set_irq(1);
}

// next DRQ block of the data in buf, limited by the byte count the host set (MAME atapi_hle_device::fill_buffer)
static void next_chunk(void)
{
	uint32_t n = cd.len - cd.pos;
	if (cd.limit < n)
		n = cd.limit - cd.limit % 2048;
	if (!n) {
		complete();
		return;
	}
	cd.chunk_end = cd.pos + n;
	alog("atapi drq %u bytes (of %u)%s\n", n, cd.len - cd.pos, cd.dma ? " dma" : "");
	cd.cyl_lo = (uint8_t)n;
	cd.cyl_hi = (uint8_t)(n >> 8);
	cd.seccount = IR_IO;
	cd.status = ST_DRDY | ST_DSC | ST_DRQ;
	if (!cd.dma) // (a DMA block raises DMARQ instead)
		set_irq(1);
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
	set_irq(1);
}

static void put_ata_string(uint8_t *w, const char *s, int bytes)
{
	for (int i = 0; i < bytes; i++)
		w[i ^ 1] = (uint8_t)(i < (int)strlen(s) ? s[i] : ' ');
}

static void packet(void)
{
	const uint8_t *p = cd.packet;
	const uint32_t lba = (uint32_t)p[2] << 24 | p[3] << 16 | p[4] << 8 | p[5];
	memset(cd.buf, 0, 512);
	alog("atapi cmd %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3], p[4], p[5],
		p[6], p[7], p[8], p[9], p[10], p[11]);
	if (!cd.chd && p[0] != 0x03 && p[0] != 0x12) { // no disc image
		fail(2, 0x3a);
		return;
	}
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
	case 0x12: // INQUIRY (MAME matsushita_cr589_device::ReadData)
		cd.buf[0] = 5;
		cd.buf[1] = 0x80;
		cd.buf[3] = 0x21;
		cd.buf[4] = 31;
		memcpy(cd.buf + 8, "MATSHITACD-ROM CR-589   GS0N", 28);
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
			const uint8_t *f = sector_data(lba + i);
			if (!f) { fail(5, 0x21); return; }
			memcpy(cd.buf + i * 2048, f, 2048);
		}
		data_in(n * 2048);
		return;
	}
	case 0xbe: { // READ CD: byte 9 picks sync (0x80), header (0x20), user data (0x10), EDC/ECC (0x08) of each sector
		const uint32_t n = (uint32_t)(p[6] << 16 | p[7] << 8 | p[8]);
		const int raw = cd.data_offset != 0, f = p[9];
		// (mode 1: sync 0-11, header 12-15, data 16-2063, EDC/ECC 2064-2351; a cooked image has the data alone)
		const int start = !raw ? 0 : (f & 0x80) ? 0 : (f & 0x20) ? 12 : 16;
		const int end = !raw ? ((f & 0x10) ? 2048 : 0) : (f & 0x08) ? 2352 : (f & 0x10) ? 2064 : (f & 0x20) ? 16 : (f & 0x80) ? 12 : 16;
		const uint32_t len = end > start ? (uint32_t)(end - start) : 0;
		if (n * len > BUF_MAX) { fail(5, 0x24); return; }
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *d = sector_data(lba + i);
			if (!d) { fail(5, 0x21); return; }
			memcpy(cd.buf + i * len, d - cd.data_offset + start, len);
		}
		data_in(n * len);
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
	set_irq(0);
	switch (c) {
	case 0xa0: // PACKET (CR-589: DRQ without INTRQ)
		cd.phase = PH_PACKET;
		cd.packet_pos = 0;
		cd.dma = cd.features & 1;
		cd.seccount = IR_COD;
		cd.status = ST_DRDY | ST_DSC | ST_DRQ;
		return;
	case 0xa1: { // IDENTIFY PACKET DEVICE (MAME matsushita_cr589_device::device_start)
		memset(cd.buf, 0, 512);
		cd.buf[0] = 0x00; cd.buf[1] = 0x85;
		put_ata_string(cd.buf + 46, "1.0", 8);
		put_ata_string(cd.buf + 54, "MATSHITA CR-589", 40);
		cd.buf[99] = 0x03; // (word 49: DMA and LBA, as MAME's atapi_cdrom_device)
		cd.len = 512;
		cd.pos = 0;
		cd.chunk_end = 512;
		cd.dma = 0;
		cd.phase = PH_DATA_IN;
		cd.seccount = IR_IO;
		cd.status = ST_DRDY | ST_DSC | ST_DRQ;
		set_irq(1);
		return;
	}
	case 0x08: // DEVICE RESET
	case 0x90: // EXECUTE DEVICE DIAGNOSTIC
		s10_atapi_reset();
		return;
	case 0xec: // IDENTIFY DEVICE: aborted with the packet signature
		signature();
		cd.error = 4;
		cd.status = ST_DRDY | ST_ERR;
		set_irq(1);
		return;
	case 0xe5: // CHECK POWER MODE: active or idle
		cd.seccount = 0xff;
		cd.status = ST_DRDY | ST_DSC;
		set_irq(1);
		return;
	case 0xef: // SET FEATURES
	case 0x00: // NOP
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: // STANDBY / IDLE (IMMEDIATE)
		cd.status = ST_DRDY | ST_DSC;
		set_irq(1);
		return;
	}
	alog("atapi: unsupported ata command %02x\n", c);
	cd.error = 4;
	cd.status = ST_DRDY | ST_ERR;
	set_irq(1);
}

int s10_atapi_dmarq(void)
{
	return cd.dma && cd.phase == PH_DATA_IN && cd.pos < cd.chunk_end;
}

uint16_t s10_atapi_r(int reg)
{
	if (!selected() && (reg == 0 || reg == 7))
		return 0;
	switch (reg) {
	case 0:
		if (cd.phase == PH_DATA_IN && cd.pos < cd.chunk_end) {
			const uint16_t v = (uint16_t)(cd.buf[cd.pos] | cd.buf[cd.pos + 1] << 8);
			cd.pos += 2;
			if (cd.pos >= cd.chunk_end) {
				if (cd.pos >= cd.len)
					complete();
				else
					next_chunk();
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
	case 7:
		set_irq(0);
		return cd.status;
	}
	return 0;
}

void s10_atapi_w(int reg, uint16_t v)
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
	case 7:
		if (selected())
			command((uint8_t)v);
		return;
	}
}

uint32_t s10_atapi_state_size(void)
{
	return cd.chd ? (uint32_t)(sizeof(cd) - CD_STATE_START) : 0;
}

void s10_atapi_state_save(void *dst)
{
	if (cd.chd)
		memcpy(dst, (const uint8_t *)&cd + CD_STATE_START, sizeof(cd) - CD_STATE_START);
}

void s10_atapi_state_load(const void *src)
{
	if (cd.chd)
		memcpy((uint8_t *)&cd + CD_STATE_START, src, sizeof(cd) - CD_STATE_START);
}
