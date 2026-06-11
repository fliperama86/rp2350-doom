//
// Host soak harness for the music-runaway bug: drives the REAL i_oplmusic.c
// + emu8950.c (compiled with the exact device flag set, minus the ARM asm)
// from a pristine WAD, renders minutes of audio per second of wall time,
// and reports the output peak per song-second. A "runaway" is the music
// peak pinning near full scale (matches PK=32767 on the device overlay).
//
// Build (from repo root):
//   cc -O2 -o /tmp/opl_soak \
//      -Isrc -Iopl -Ibuild-host -DEMU_HARNESS=1 \
//      -DUSE_EMU8950_OPL=1 -DEMU8950_NO_RATECONV=1 -DEMU8950_LINEAR=1 \
//      -DEMU8950_SLOT_RENDER=1 -DEMU8950_LINEAR_SKIP=1 \
//      -DEMU8950_LINEAR_END_OF_NOTE_OPTIMIZATION \
//      -DEMU8950_NO_PERCUSSION_MODE=1 -DEMU8950_NO_WAVE_TABLE_MAP=1 \
//      -DEMU8950_NO_TLL=1 -DEMU8950_NO_FLOAT=1 -DEMU8950_NO_TIMER=1 \
//      -DEMU8950_NO_TEST_FLAG=1 -DEMU8950_SIMPLER_NOISE=1 \
//      -DEMU8950_SHORT_NOISE_UPDATE_CHECK=1 \
//      -fms-extensions \
//      tools/opl_soak/opl_soak.c src/i_oplmusic.c opl/emu8950.c \
//      opl/opl_queue.c src/midifile.c src/mus2mid.c src/memio.c
//
// Run: /tmp/opl_soak ~/Downloads/DOOM1.WAD [minutes_per_song]
//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "i_sound.h"
#include "w_wad.h"
#include "opl.h"
#include "opl_queue.h"
#include "emu8950.h"

#define SAMPLE_RATE 49716 // chip-native; the rate constant is irrelevant to the bug (bisected)

// ===========================================================================
// Minimal WAD loader + W_* shims
// ===========================================================================

typedef struct { char name[9]; uint32_t pos, size; } wadlump_t;
static wadlump_t *lumps;
static int num_lumps;
static uint8_t *wad_data;

static void load_wad(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    wad_data = malloc(sz);
    fread(wad_data, 1, sz, f);
    fclose(f);
    num_lumps = *(int32_t *)(wad_data + 4);
    uint32_t diroff = *(uint32_t *)(wad_data + 8);
    lumps = calloc(num_lumps, sizeof(wadlump_t));
    for (int i = 0; i < num_lumps; i++) {
        uint8_t *e = wad_data + diroff + i * 16;
        lumps[i].pos = *(uint32_t *)e;
        lumps[i].size = *(uint32_t *)(e + 4);
        memcpy(lumps[i].name, e + 8, 8);
    }
}

lumpindex_t W_GetNumForName(const char *name)
{
    for (int i = 0; i < num_lumps; i++) {
        if (!strncasecmp(lumps[i].name, name, 8)) return i;
    }
    fprintf(stderr, "lump %s not found\n", name);
    exit(1);
}

int W_LumpLength(lumpindex_t l) { return lumps[l].size; }
void *W_CacheLumpNum(lumpindex_t l, int tag) { (void)tag; return wad_data + lumps[l].pos; }
void *W_CacheLumpName(const char *name, int tag) { return W_CacheLumpNum(W_GetNumForName(name), tag); }
void W_ReleaseLumpNum(lumpindex_t l) { (void)l; }
void W_ReleaseLumpName(const char *name) { (void)name; }

// ===========================================================================
// Misc shims
// ===========================================================================

const char *DEH_String(const char *s) { return s; }
void I_Error(const char *fmt, ...) { fprintf(stderr, "I_Error: %s\n", fmt); exit(1); }
boolean M_FileExists(const char *path) { (void)path; return false; }
char *M_TempFile(const char *s) { (void)s; return strdup("/tmp/opl_soak_tmp"); }
boolean M_WriteFile(const char *name, const void *src, int len)
{
    FILE *f = fopen(name, "wb");
    if (!f) return false;
    fwrite(src, 1, len, f);
    fclose(f);
    return true;
}
int M_snprintf(char *buf, size_t buf_len, const char *fmt, ...) { (void)fmt; if (buf_len) buf[0] = 0; return 0; }
void *Z_Malloc_(int size, int tag, void *user) { (void)tag; (void)user; return malloc(size); }
void *Z_Malloc(int size, int tag, void *user) { (void)tag; (void)user; return malloc(size); }
void Z_Free(void *p) { free(p); }
void *I_Realloc(void *ptr, size_t size) { return realloc(ptr, size); }
boolean M_StringConcat(char *dest, const char *src, size_t dest_size)
{
    strncat(dest, src, dest_size - strlen(dest) - 1);
    return true;
}
int snd_samplerate = SAMPLE_RATE;

// ===========================================================================
// OPL layer: emu8950 + callback queue, mirroring opl_pico.c semantics
// ===========================================================================

static OPL *emu;
static opl_callback_queue_t *queue;
static uint64_t current_time; // us (OPL_SECOND = 1e6)
static int opl_paused;
static uint64_t pause_offset;

opl_init_result_t OPL_Init(unsigned int port_base)
{
    (void)port_base;
    emu = OPL_new(3579552, SAMPLE_RATE);
    queue = OPL_Queue_Create();
    return OPL_INIT_OPL2;
}

void OPL_Shutdown(void) {}
void OPL_SetSampleRate(unsigned int rate) { (void)rate; }
void OPL_WritePort(opl_port_t port, unsigned int value) { (void)port; (void)value; }
unsigned int OPL_ReadPort(opl_port_t port) { (void)port; return 0; }
unsigned int OPL_ReadStatus(void) { return 0; }
void OPL_WriteRegister(int reg, int value) { OPL_writeReg(emu, reg, value); }
opl_init_result_t OPL_Detect(void) { return OPL_INIT_OPL2; }

void OPL_InitRegisters(int opl3)
{
    (void)opl3;
    for (int r = 0x01; r <= 0xF5; r++) OPL_WriteRegister(r, 0);
    OPL_WriteRegister(0x01, 0x20); // wave select enable
}

void OPL_SetCallback(uint64_t us, opl_callback_t callback, void *data)
{
    OPL_Queue_Push(queue, callback, data, current_time - pause_offset + us);
}

void OPL_AdjustCallbacks(unsigned int old_tempo, unsigned int new_tempo)
{
    OPL_Queue_AdjustCallbacks(queue, current_time, old_tempo, new_tempo);
}

void OPL_ClearCallbacks(void) { OPL_Queue_Clear(queue); }
void OPL_Lock(void) {}
void OPL_Unlock(void) {}
void OPL_Delay(uint64_t us) { (void)us; }
void OPL_SetPaused(int paused) { opl_paused = paused; }

// AdvanceTime, faithful to opl_pico.c
static void advance_time(unsigned int nsamples)
{
    uint64_t us = ((uint64_t)nsamples * OPL_SECOND) / SAMPLE_RATE;
    current_time += us;
    if (opl_paused) pause_offset += us;

    opl_callback_t callback;
    void *callback_data;
    while (!OPL_Queue_IsEmpty(queue)
           && current_time >= OPL_Queue_Peek(queue) + pause_offset) {
        if (!OPL_Queue_Pop(queue, &callback, &callback_data)) break;
        callback(callback_data);
    }
}

// Render like OPL_Pico_Mix_callback: segment by next-callback time.
static void render(int32_t *buf, unsigned int nsamples)
{
    unsigned int filled = 0;
    while (filled < nsamples) {
        uint64_t nsamp;
        if (opl_paused || OPL_Queue_IsEmpty(queue)) {
            nsamp = nsamples - filled;
        } else {
            uint64_t next_t = OPL_Queue_Peek(queue) + pause_offset;
            nsamp = (next_t - current_time) * SAMPLE_RATE;
            nsamp = (nsamp + OPL_SECOND - 1) / OPL_SECOND;
            if (nsamp > nsamples - filled) nsamp = nsamples - filled;
        }
        if (nsamp) OPL_calc_buffer_stereo(emu, buf + filled, nsamp);
        filled += nsamp;
        advance_time(nsamp);
    }
}

// ===========================================================================
// Main: play each song, watch the peak (device-equivalent: <<3 saturated)
// ===========================================================================

extern const music_module_t music_opl_module;

// In MUSX mode (-DUSE_MUSX=1), songs load from <musxdir>/<NAME>.musx --
// the exact device format/path -- instead of the WAD's MUS lumps.
static void *load_musx(const char *dir, const char *name, int *len)
{
    char path[512];
    snprintf(path, sizeof path, "%s/%s.musx", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END); *len = (int)ftell(f); fseek(f, 0, SEEK_SET);
    void *buf = malloc(*len);
    fread(buf, 1, *len, f);
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <wad> [minutes] [musxdir]\n", argv[0]); return 1; }
    int minutes = argc > 2 ? atoi(argv[2]) : 5;
    const char *musxdir = argc > 3 ? argv[3] : NULL;
    load_wad(argv[1]);

    if (!music_opl_module.Init()) { fprintf(stderr, "music init failed\n"); return 1; }
    music_opl_module.SetMusicVolume(127);

    static const char *songs[] = {
        "D_E1M1", "D_E1M2", "D_E1M3", "D_E1M4", "D_E1M5",
        "D_E1M6", "D_E1M7", "D_E1M8", "D_E1M9",
        "D_INTER", "D_INTRO", "D_VICTOR",
    };
    enum { CHUNK = 128 };
    static int32_t buf[CHUNK];

    for (unsigned s = 0; s < sizeof(songs) / sizeof(songs[0]); s++) {
        void *song_data;
        int song_len;
        if (musxdir) {
            song_data = load_musx(musxdir, songs[s], &song_len);
            if (!song_data) continue;
        } else {
            lumpindex_t l = W_GetNumForName(songs[s]);
            song_data = W_CacheLumpNum(l, 0);
            song_len = W_LumpLength(l);
        }
        void *handle = music_opl_module.RegisterSong(song_data, song_len);
        if (!handle) { printf("%s: register failed\n", songs[s]); continue; }
        music_opl_module.PlaySong(handle, 1);

        // Distinguish ordinary x8-gain clipping (brief peaks over 4095 raw)
        // from the device's catastrophic runaway (RAW output pinned huge for
        // many consecutive seconds, KeyOff-immune).
        int max_raw = 0;
        int pinned_run = 0, longest_pinned = 0, pinned_start = -1;
        for (int sec = 0; sec < minutes * 60; sec++) {
            int raw_peak = 0;
            for (int c = 0; c < SAMPLE_RATE / CHUNK; c++) {
                render(buf, CHUNK);
                for (int i = 0; i < CHUNK; i++) {
                    int v = (int16_t)(buf[i] & 0xffff);
                    if (v < 0) v = -v;
                    if (v > raw_peak) raw_peak = v;
                }
            }
            if (raw_peak > max_raw) max_raw = raw_peak;
            if (raw_peak >= 8192) { // double the clip threshold = abnormal
                if (pinned_run == 0) pinned_start = sec;
                pinned_run++;
                if (pinned_run > longest_pinned) longest_pinned = pinned_run;
            } else {
                pinned_run = 0;
            }
        }
        printf("%s: max_raw=%d longest_abnormal_run=%ds (from t=%ds) %s\n",
               songs[s], max_raw, longest_pinned, pinned_start,
               longest_pinned >= 5 ? "*** RUNAWAY ***" : "(clip-only or clean)");
        music_opl_module.StopSong();
        music_opl_module.UnRegisterSong(handle);
    }
    return 0;
}
