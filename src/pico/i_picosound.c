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
//

#include "config.h"

#include <string.h>
#include <assert.h>
#include <doom/sounds.h>
#include <z_zone.h>

#include "deh_str.h"
#include "i_sound.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomtype.h"
#include "i_picosound.h"
// #include "pico/audio_i2s.h" // Audio hardware disabled
#include "pico/binary_info.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define ADPCM_BLOCK_SIZE 128
#define ADPCM_SAMPLES_PER_BLOCK_SIZE 249
#define LOW_PASS_FILTER
#define MIX_MAX_VOLUME 128
typedef struct channel_s channel_t;

static volatile enum {
    FS_NONE,
    FS_FADE_OUT,
    FS_FADE_IN,
    FS_SILENT,
} fade_state;
#define FADE_STEP 8 // must be power of 2
uint16_t fade_level;

struct channel_s
{
    const uint8_t *data;
    const uint8_t *data_end;
    uint32_t offset;
    uint32_t step;
    uint8_t left, right; // 0-255
    uint8_t decompressed_size;
#if SOUND_LOW_PASS
    uint8_t alpha256;
#endif
    int8_t decompressed[ADPCM_SAMPLES_PER_BLOCK_SIZE];
};

// HDMI audio output: Core 0 mixes SFX + music into a lock-free ring of
// interleaved stereo pairs; Core 1's command-list data-island writer pulls
// exactly 800 pairs per video frame (48 kHz) via I_PicoSoundPullStereo().
#ifndef PICODOOM_HDMI_LITE
#define PICODOOM_HDMI_LITE 0
#endif

#define AUDIO_RING_PAIRS 2048u // power of two; ~43 ms of buffering
#if PICODOOM_HDMI_LITE
// Keep the 8 KB ring out of BSS so the zone keeps its headroom: park it in
// the free top-of-SRAM region (layout + overlap asserts in
// hdmi_lite_layout.h).
#include "hdmi_lite_layout.h"
static int16_t *const audio_ring = (int16_t *)HDMI_LITE_AUDIO_RING_ADDR;
_Static_assert(AUDIO_RING_PAIRS * 2 * sizeof(int16_t) <= HDMI_LITE_AUDIO_RING_BYTES,
               "audio ring overflows its region slot");
#else
static int16_t audio_ring[AUDIO_RING_PAIRS * 2];
#endif
static volatile uint32_t audio_ring_head; // free-running, producer (Core 0)
static volatile uint32_t audio_ring_tail; // free-running, consumer (Core 1)

#define MIX_CHUNK_PAIRS 128u
static int16_t mix_chunk[MIX_CHUNK_PAIRS * 2];
static audio_buffer_bytes_t mix_chunk_bytes = {
        .bytes = (uint8_t *)mix_chunk,
        .size = sizeof(mix_chunk),
};
static audio_buffer_t mix_chunk_buffer = {
        .buffer = &mix_chunk_bytes,
        .max_sample_count = MIX_CHUNK_PAIRS,
};

// ====== FROM ADPCM-LIB =====
#define CLIP(data, min, max) \
if ((data) > (max)) data = max; \
else if ((data) < (min)) data = min;

/* step table */
static const uint16_t step_table[89] = {
        7, 8, 9, 10, 11, 12, 13, 14,
        16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66,
        73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658,
        724, 796, 876, 963, 1060, 1166, 1282, 1411,
        1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024,
        3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
        7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
        32767
};

/* step index tables */
static const int index_table[] = {
        /* adpcm data size is 4 */
        -1, -1, -1, -1, 2, 4, 6, 8
};
// =============================

static void (*music_generator)(audio_buffer_t *buffer);

static boolean sound_initialized = false;
static channel_t channels[NUM_SOUND_CHANNELS];

static boolean use_sfx_prefix;

static inline bool is_channel_playing(int channel) {
    return channels[channel].decompressed_size != 0;
}

static inline void stop_channel(int channel) {
    channels[channel].decompressed_size = 0;
}

static bool check_and_init_channel(int channel) {
    return sound_initialized && ((uint)channel) < NUM_SOUND_CHANNELS;
}

int adpcm_decode_block_s8(int8_t *outbuf, const uint8_t *inbuf, int inbufsize)
{
#if 1
    int samples = 1, chunks;

    if (inbufsize < 4)
        return 0;

    int32_t pcmdata = (int16_t) (inbuf [0] | (inbuf [1] << 8));
    *outbuf++ = pcmdata>>8u;
    int index = inbuf[2];

    if (index < 0 || index > 88 || inbuf [3])     // sanitize the input a little...
        return 0;

    inbufsize -= 4;
    inbuf += 4;

    chunks = inbufsize / 4;
    samples += chunks * 8;

    while (chunks--) {
        for (int i = 0; i < 4; ++i) {
            int step = step_table[index], delta = step >> 3;

            if (*inbuf & 1) delta += (step >> 2);
            if (*inbuf & 2) delta += (step >> 1);
            if (*inbuf & 4) delta += step;
            if (*inbuf & 8) delta = -delta;

            pcmdata += delta;
            index += index_table [*inbuf & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2] = pcmdata>>8u;

            step = step_table[index], delta = step >> 3;

            if (*inbuf & 0x10) delta += (step >> 2);
            if (*inbuf & 0x20) delta += (step >> 1);
            if (*inbuf & 0x40) delta += step;
            if (*inbuf & 0x80) delta = -delta;

            pcmdata += delta;
            index += index_table[(*inbuf >> 4) & 0x7];
            CLIP(index, 0, 88);
            CLIP(pcmdata, -32768, 32767);
            outbuf [i * 2 + 1] = pcmdata>>8u;
            inbuf++;
        }

        outbuf += 8;
    }

    return samples;
#else
    extern int adpcm_decode_block (int16_t *outbuf, const uint8_t *inbuf, size_t inbufsize, int channels);
    static int16_t tmp[ADPCM_SAMPLES_PER_BLOCK_SIZE];
    int samples = adpcm_decode_block(tmp, inbuf, inbufsize, 1);
    for(int s=0;s<samples;s++) {
        outbuf[s] = tmp[s] / 256;
    }
    return samples;
#endif
}

static void decompress_buffer(channel_t *channel) {
    if (channel->data == channel->data_end) {
        channel->decompressed_size = 0;
    } else {
        int block_size = MIN(ADPCM_BLOCK_SIZE, channel->data_end - channel->data);
        channel->decompressed_size = adpcm_decode_block_s8(channel->decompressed, channel->data, block_size);
        assert(channel->decompressed_size && channel->decompressed_size <= sizeof(channel->decompressed));
        channel->data += block_size;
    }
}

static boolean init_channel_for_sfx(channel_t *ch, const sfxinfo_t *sfxinfo, int pitch)
{
    int lumpnum = sfx_mut(sfxinfo)->lumpnum;
    int lumplen = W_LumpLength(lumpnum);

    const uint8_t *data = W_CacheLumpNum(lumpnum, PU_STATIC); // we don't track because we assume in ROWAD anyway

    if (lumplen < 8 || data[0] != 0x03 || data[1] != 0x80) // note 0x80 i.e. only support compressed right now
    {
        return false;
    }

    // 16 bit sample rate field, 32 bit length field

//    int length = (data[7] << 24) | (data[6] << 16) | (data[5] << 8) | data[4];
//    length -= 40; // 8 for header, 32 because we didn't updated it in lump converter (which cuts of unused 16 bit leading/leadout)
//    if (length <= 0) {
//        return false;
//    }
    int length = lumplen - 8;
//    printf("channel %d lump %d size %d at %p len2 %d\n", (int)(ch-channels), lumpnum, lumplen, data, length);

    ch->data = data + 8;
    ch->data_end = ch->data + length;

    uint32_t sample_freq = (data[3] << 8) | data[2];
    if (pitch == NORM_PITCH)
        ch->step = sample_freq * 65536 / PICO_SOUND_SAMPLE_FREQ;
    else
        ch->step = (uint32_t)((sample_freq * pitch) * 65536ull / (PICO_SOUND_SAMPLE_FREQ * pitch));

    decompress_buffer(ch); // we need non-zero decompressed size if playing
    ch->offset = 0;

#if SOUND_LOW_PASS
//    const float dt = 1.0f / PICO_SOUND_SAMPLE_FREQ;
//    const float rc = 1.0f / (3.14f * sample_freq);
//    const float alpha = dt / (rc + dt);
//    ch->alpha256 = (int)(256*alpha);
    ch->alpha256 = 256u * 201u * sample_freq / (201u * sample_freq + 64u * (uint)PICO_SOUND_SAMPLE_FREQ);
#endif
    return true;
}

static void GetSfxLumpName(const sfxinfo_t *sfx, char *buf, size_t buf_len)
{
    // Linked sfx lumps? Get the lump number for the sound linked to.
    if (sfx->link != NULL)
    {
        sfx = sfx->link;
    }

    // Doom adds a DS* prefix to sound lumps; Heretic and Hexen don't
    // do this.

    if (use_sfx_prefix)
    {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    }
    else
    {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

static void I_Pico_PrecacheSounds(should_be_const sfxinfo_t *sounds, int num_sounds)
{
    // no-op
}

static int I_Pico_GetSfxLumpNum(should_be_const sfxinfo_t *sfx)
{
    char namebuf[9];
    GetSfxLumpName(sfx, namebuf, sizeof(namebuf));
    return W_GetNumForName(namebuf);
}

static void I_Pico_UpdateSoundParams(int handle, int vol, int sep)
{
    int left, right;

    if (!sound_initialized || handle < 0 || handle >= NUM_SOUND_CHANNELS)
    {
        return;
    }

    // todo graham seems unnecessary
    left = ((254 - sep) * vol) / 127;
    right = ((sep) * vol) / 127;

    if (left < 0) left = 0;
    else if ( left > 255) left = 255;
    if (right < 0) right = 0;
    else if (right > 255) right = 255;

    channels[handle].left = left;
    channels[handle].right = right;
}

extern volatile uint32_t hdmi_diag_checkpoint;

static int I_Pico_StartSound(should_be_const sfxinfo_t *sfxinfo, int channel, int vol, int sep, int pitch)
{
    if (!check_and_init_channel(channel)) return -1;

    hdmi_diag_checkpoint = 50;
    stop_channel(channel);
    channel_t *ch = &channels[channel];
    if (!init_channel_for_sfx(ch, sfxinfo, pitch)) {
        assert(!is_channel_playing(channel)); // don't expect to have to mark it sotpped
    }
    I_Pico_UpdateSoundParams(channel, vol, sep);
    hdmi_diag_checkpoint = 51;
    return channel;
}

static void I_Pico_StopSound(int channel)
{
    if (check_and_init_channel(channel)) {

    }
}

static boolean I_Pico_SoundIsPlaying(int channel)
{
    if (!check_and_init_channel(channel)) return false;
    return is_channel_playing(channel);
}

// Mix one MIX_CHUNK_PAIRS chunk of music + SFX into mix_chunk.
// Mixing logic matches upstream rp2040-doom's I2S buffer fill.
static void mix_one_chunk(void)
{
    if (music_generator) {
        hdmi_diag_checkpoint = 62;
        music_generator(&mix_chunk_buffer);
        hdmi_diag_checkpoint = 63;
    } else {
        memset(mix_chunk, 0, sizeof(mix_chunk));
    }
    for (int ch = 0; ch < NUM_SOUND_CHANNELS; ch++) {
        if (is_channel_playing(ch)) {
            channel_t *channel = &channels[ch];
            assert(channel->decompressed_size);
            int voll = channel->left / 2;
            int volr = channel->right / 2;
            uint offset_end = channel->decompressed_size * 65536;
            assert(channel->offset < offset_end);
            int16_t *samples = mix_chunk;
#if SOUND_LOW_PASS
            int alpha256 = channel->alpha256;
            int beta256 = 256 - alpha256;
            int sample = channel->decompressed[channel->offset >> 16];
#endif
            for (uint s = 0; s < MIX_CHUNK_PAIRS; s++) {
#if !SOUND_LOW_PASS
                int sample = channel->decompressed[channel->offset >> 16];
#else
                sample = (beta256 * sample + alpha256 * channel->decompressed[channel->offset >> 16]) / 256;
#endif
                *samples++ += sample * voll;
                *samples++ += sample * volr;
                channel->offset += channel->step;
                if (channel->offset >= offset_end) {
                    channel->offset -= offset_end;
                    decompress_buffer(channel);
                    offset_end = channel->decompressed_size * 65536;
                    if (channel->offset >= offset_end) {
                        stop_channel(ch);
                        break;
                    }
                }
            }
        }
    }
    if (fade_state == FS_SILENT) {
        memset(mix_chunk, 0, sizeof(mix_chunk));
    } else if (fade_state != FS_NONE) {
        int16_t *samples = mix_chunk;
        int fade_step = fade_state == FS_FADE_IN ? FADE_STEP : -FADE_STEP;
        uint i;
        for (i = 0; i < MIX_CHUNK_PAIRS * 2 && fade_level; i += 2) {
            samples[i] = (samples[i] * (int)fade_level) >> 16;
            samples[i + 1] = (samples[i + 1] * (int)fade_level) >> 16;
            fade_level += fade_step;
        }
        if (!fade_level) {
            if (fade_state == FS_FADE_OUT) {
                for (; i < MIX_CHUNK_PAIRS * 2; i++) {
                    samples[i] = 0;
                }
                fade_state = FS_SILENT;
            } else {
                fade_state = FS_NONE;
            }
        }
    }
}

static void I_Pico_UpdateSound(void)
{
    if (!sound_initialized) return;

    // Top up the ring; the HDMI side drains exactly 800 pairs per 60 Hz frame.
    hdmi_diag_checkpoint = 60;
    while (AUDIO_RING_PAIRS - (audio_ring_head - audio_ring_tail) >= MIX_CHUNK_PAIRS) {
        mix_one_chunk();
        uint32_t head = audio_ring_head;
        uint32_t idx = head & (AUDIO_RING_PAIRS - 1);
        uint32_t first = MIN(MIX_CHUNK_PAIRS, AUDIO_RING_PAIRS - idx);
        memcpy(&audio_ring[idx * 2], mix_chunk, first * 2 * sizeof(int16_t));
        if (first < MIX_CHUNK_PAIRS) {
            memcpy(audio_ring, &mix_chunk[first * 2], (MIX_CHUNK_PAIRS - first) * 2 * sizeof(int16_t));
        }
        __mem_fence_release();
        audio_ring_head = head + MIX_CHUNK_PAIRS;
    }
    hdmi_diag_checkpoint = 61;
}

int I_PicoSoundPullStereo(int16_t *dst, int sample_pairs)
{
    uint32_t tail = audio_ring_tail;
    uint32_t avail = audio_ring_head - tail;
    __mem_fence_acquire();
    uint32_t n = MIN(avail, (uint32_t)sample_pairs);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t idx = (tail + i) & (AUDIO_RING_PAIRS - 1);
        dst[i * 2] = audio_ring[idx * 2];
        dst[i * 2 + 1] = audio_ring[idx * 2 + 1];
    }
    if ((int)n < sample_pairs) {
        memset(&dst[n * 2], 0, (sample_pairs - n) * 2 * sizeof(int16_t));
    }
    audio_ring_tail = tail + n;
    return (int)n;
}

static void I_Pico_ShutdownSound(void)
{
    if (!sound_initialized)
    {
        return;
    }
    sound_initialized = false;
}

static boolean I_Pico_InitSound(boolean _use_sfx_prefix)
{
    use_sfx_prefix = _use_sfx_prefix;
    // No audio hardware to set up: output is HDMI data islands fed from the
    // sample ring by Core 1's command-list writer.
    sound_initialized = true;
    return true;
}

static snddevice_t sound_pico_devices[] =
{
    SNDDEVICE_SB,
};

sound_module_t sound_pico_module =
{
    sound_pico_devices,
    arrlen(sound_pico_devices),
    I_Pico_InitSound,
    I_Pico_ShutdownSound,
    I_Pico_GetSfxLumpNum,
    I_Pico_UpdateSound,
    I_Pico_UpdateSoundParams,
    I_Pico_StartSound,
    I_Pico_StopSound,
    I_Pico_SoundIsPlaying,
    I_Pico_PrecacheSounds,
};

bool I_PicoSoundIsInitialized(void) {
    return sound_initialized;
}

// Diagnostic accessors (e.g. for on-screen audio status stripes).
uint32_t I_PicoSoundMixedCount(void) {
    return audio_ring_head;
}

uint32_t I_PicoSoundPulledCount(void) {
    return audio_ring_tail;
}

bool I_PicoSoundMusicActive(void) {
    return music_generator != NULL;
}

void I_PicoSoundSetMusicGenerator(void (*generator)(audio_buffer_t *buffer)) {
#if PICODOOM_NO_MUSIC
    // G4 gate: SFX-only mixer. Song registration and level transitions
    // still run; the generator is just never installed.
    (void)generator;
#else
    music_generator = generator;
#endif
}

#if PICO_ON_DEVICE
void I_PicoSoundFade(bool in) {
    fade_state = in ? FS_FADE_IN : FS_FADE_OUT;
    fade_level = in ? FADE_STEP : 0x10000 - FADE_STEP;
}

bool I_PicoSoundFading(void) {
    return fade_state == FS_FADE_IN || fade_state == FS_FADE_OUT;
}
#endif
