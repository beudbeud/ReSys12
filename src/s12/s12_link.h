// ReSys12: kartduel link board CPU (s12_link.c)
#ifndef S12_LINK_H
#define S12_LINK_H

#include <stdint.h>

extern uint16_t s12_link_ram[0x8000]; // shared RAM, main CPU 0x1f780000-0x1f78ffff, link CPU 0x000000-0x00ffff
void s12_link_reset(void);            // board reset: link CPU held in reset, RAM cleared
void s12_link_sync(void);             // run the link CPU up to the main CPU time
void s12_link_reset_line(int release); // main CPU write to 0x1f796000 (assert) / 0x1f796002 (release)

// cabinet link (the frontend's transport): frames the C422 transmits, and a poll for the ones that arrived, which it
// hands to s12_link_receive
extern void (*s12_link_send_hook)(const uint8_t *data, int count);
extern void (*s12_link_poll_hook)(void);
void s12_link_receive(const uint8_t *data, int count);

uint32_t s12_link_state_size(void);
void s12_link_state_save(void *dst);
void s12_link_state_load(const void *src);

#endif
