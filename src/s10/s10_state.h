// ReSys10: save states of the whole machine (core + board). GPL-2.0-or-later.
#ifndef S10_STATE_H
#define S10_STATE_H

#include <stddef.h>
#include <stdint.h>

void s10_state_set_game(const char *name);   // the set name written in / checked against the header
uint8_t *s10_state_save_buf(size_t *size);   // malloc'd, NULL on failure
int s10_state_load_buf(const uint8_t *buf, size_t size); // 0 = ok

#endif
