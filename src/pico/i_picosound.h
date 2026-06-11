//
// Copyright(C) 1993-1996 Id Software, Inc.
// Copyright(C) 2005-2014 Simon Howard
// Copyright(C) 2008 David Flater
// Copyright(C) 2021-2022 Graham Sanderson
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//	System interface for sound.

#ifndef __I_PICO_SOUND__
#define __I_PICO_SOUND__

#include "pico.h"

// Minimal audio_buffer_t shim (I2S audio hardware disabled)
typedef struct audio_buffer_bytes {
    uint8_t *bytes;
    uint32_t size;
} audio_buffer_bytes_t;

typedef struct audio_buffer {
    audio_buffer_bytes_t *buffer;
    uint32_t max_sample_count;
    uint32_t sample_count;
} audio_buffer_t;

// HDMI audio is clocked at 48 kHz (exactly 800 samples per 60 Hz frame at
// the 25.2 MHz pixel clock); both OPL engines accept an arbitrary rate.
#define PICO_SOUND_SAMPLE_FREQ 48000

#ifndef NUM_SOUND_CHANNELS
// this is the defaul tin game not 16
#define NUM_SOUND_CHANNELS 8
#endif

void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer));
bool I_PicoSoundIsInitialized(void);
// Drain mixed stereo samples (interleaved L/R pairs); zero-fills on underrun.
// Called from Core 1's HDMI data-island writer. Returns pairs actually mixed.
int I_PicoSoundPullStereo(int16_t *dst, int sample_pairs);
// Diagnostics: total pairs mixed/pulled so far / whether music is set.
uint32_t I_PicoSoundMixedCount(void);
uint32_t I_PicoSoundPulledCount(void);
bool I_PicoSoundMusicActive(void);
void I_PicoSoundFade(bool in);
bool I_PicoSoundFading(void);
#endif
