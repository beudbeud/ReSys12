// ReSys12: CDXA board audio (37800 Hz) resampled and mixed into the C352 output. GPL-2.0-or-later.
#ifndef S12_CDMIX_H
#define S12_CDMIX_H

#include <stdint.h>

// empties the FIFO (load, reset, state load)
void s12_cdmix_clear(void);
// queues the frame's CD audio (emulation thread, while s12_cdmix_mix is not running)
void s12_cdmix_pull(void);
// adds the queued CD audio to n stereo pairs of C352 output (88.2 kHz, 44.1 kHz with s12_sound_half_rate)
void s12_cdmix_mix(int16_t *buf, int n);

#endif
