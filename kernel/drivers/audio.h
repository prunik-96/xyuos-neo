#ifndef AUDIO_H
#define AUDIO_H

#include <stdint.h>

// The system's audio output.
//
// Two back ends sit behind this: an Intel HD Audio codec when the machine has
// one (which every PC made this century does), and the PC speaker when it
// does not. The speaker cannot play a waveform, so a system sound is defined
// as a short melody rather than a sample -- that way the same call means
// something on both, instead of silently doing nothing on the fallback.

#define AUDIO_RATE 48000          // the one rate HDA guarantees
#define AUDIO_CH   2              // stereo, 16-bit signed

// Bring up whatever is present. 1 if a real codec answered, 0 if the fallback
// speaker is all there is, -1 if there is no output at all.
int  audio_init(void);
int  audio_ready(void);
const char *audio_backend(void);

// Queue signed 16-bit stereo frames for playback. Returns the number of FRAMES
// accepted, which may be fewer than asked for when the ring is full -- the
// caller is expected to come back with the rest.
int  audio_write(const int16_t *frames, int nframes);

// How many frames are still waiting to be played. A player uses this to keep
// the ring topped up without running ahead of the hardware.
int  audio_queued(void);

// Stop output and drop anything queued.
void audio_stop(void);

// Master volume, 0..100.
void audio_set_volume(int pct);
int  audio_volume(void);

// --- system sounds ---------------------------------------------------------
// Played for the message-box kinds in wm.h (MB_ERROR / MB_WARN / MB_INFO).
// Synthesised, not loaded from disk: a sound the system needs in order to
// report a failure must not itself depend on the filesystem.
void sound_alert(int kind);

// Called from the timer tick to advance the PC-speaker melody, if one is
// playing. Harmless when HDA is in use.
void audio_tick(void);

#endif
