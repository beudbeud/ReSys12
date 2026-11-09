// ReSys10: Namco System 10 board (main CPU side). Behaviour follows MAME src/mame/namco/namcos10.cpp and ns10crypt.cpp
// (BSD-3-Clause, smf, windyfairy, Andreas Naive and contributors). C interface for the frontend.
#ifndef S10_H
#define S10_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// memory board (MAME driver state class)
enum { S10_MEMM, S10_MEMN, S10_MEMP3, S10_MEMIO };
// I/O board (EXIO: TMP95C061 + I/O, MGEXIO: medal games, EXFINAL: Medal no Tatsujin)
enum { S10_IO_NONE, S10_IO_EXIO, S10_IO_MGEXIO, S10_IO_EXFINAL };
enum { S10_K9F2808U0B, S10_K9F5608U0D, S10_TC58256AFT };
enum { S10_DEC_NONE, S10_DEC_TYPE1, S10_DEC_TYPE2, S10_DEC_TYPE2_NONLINEAR };
// ROM regions: MEM(M) flash and data ROMs, decrypter table, NVRAM, MEM(N) NAND chips 0..15
enum { S10_RGN_NAND, S10_RGN_DATA, S10_RGN_NONLINEAR, S10_RGN_NVRAM, S10_RGN_NAND0, S10_RGN_COUNT = S10_RGN_NAND0 + 16 };

typedef struct { int rgn; const char *name; uint32_t offset, size, crc; } S10File;
typedef struct {
	const char *name, *parent, *desc, *year;
	int cfg;         // machine config (index into the generated config table)
	int not_working; // MAME MACHINE_NOT_WORKING
	const S10File *files;
	const char *disk; // MAME DISK_IMAGE: the CD-ROM's CHD (<content dir>/<set>/<disk>.chd), "" = none
} S10Game;

// MAME "IN1" port bits, active high here (the board inverts them)
enum {
	S10_P1_RIGHT = 1u << 0, S10_P1_LEFT = 1u << 1, S10_P1_DOWN = 1u << 2, S10_P1_UP = 1u << 3,
	S10_P1_B1 = 1u << 4, S10_P1_B2 = 1u << 5, S10_P1_B3 = 1u << 6, S10_START1 = 1u << 7,
	S10_P2_RIGHT = 1u << 8, S10_P2_LEFT = 1u << 9, S10_P2_DOWN = 1u << 10, S10_P2_UP = 1u << 11,
	S10_P2_B1 = 1u << 12, S10_P2_B2 = 1u << 13, S10_P2_B3 = 1u << 14, S10_START2 = 1u << 15,
	S10_P1_B4 = 1u << 17, S10_P1_B5 = 1u << 18, S10_P1_B6 = 1u << 19,
	S10_P2_B4 = 1u << 21, S10_P2_B5 = 1u << 22, S10_P2_B6 = 1u << 23,
	S10_G13_SCOPE = 1u << 27, // g13jnr: the scope sensor (the player's eye at the scope)
	S10_COIN2 = 1u << 28, S10_COIN1 = 1u << 29, S10_TEST = 1u << 30, S10_SERVICE = 1u << 31,
};

int s10_game_count(void);
const S10Game *s10_game(int i);
int s10_find_game(const char *name);
int s10_board_kind(const S10Game *g); // S10_MEM*
int s10_io_kind(const S10Game *g);    // S10_IO_*

// loads <dir>/<set>.zip (missing files from the parent zip) and attaches the board; 0 = ok, else msg says why
int s10_load(const char *dir, const char *name, char *msg, size_t msglen);
void s10_unload(void);

extern uint32_t s10_in1;     // S10_* bits, active high (S10_TEST is a level: the cabinet's slide switch)
extern uint32_t s10_exio_in1; // MAME EXIO_IN1, active high (nflclsfb's pass buttons)
extern uint16_t s10_dipsw;   // SW1 (MAME "SYSTEM" bits 0-7), active high = switch on
// light gun sets: the player's aim at screen fraction fx, fy (0..1, left / top = 0)
enum { S10_GUN_NONE, S10_GUN_EXIO, S10_GUN_P3 }; // ptblank3 / gunbalina: EXIO guns, g13jnr: MEM(P3) rifle (1 player)
int s10_has_gun(void);
void s10_set_gun(int player, float fx, float fy);

// the game's settings / records (MEM(N): NAND 0 block 1; MEM(M): the flash at 0x40000), for the frontend's save file
void *s10_nvram(size_t *size);
// unhandled board accesses seen (diagnostics, RS10_UNMAPPED)
void s10_dump_unmapped(void);

// board state for save states (registers, NAND chip states, decrypter, writable NAND areas)
size_t s10_state_size(void);
void s10_state_save(void *dst);
int s10_state_load(const void *src, size_t size);

#ifdef __cplusplus
}
#endif

#endif
